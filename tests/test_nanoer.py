import io
import os
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

from nanoer import Archive, CorruptChunkError, FormatError, pack
from nanoer.codecs import available_codecs
from nanoer.format import HEADER
from nanoer.server import ArchiveServer, parse_range

CHUNK = 64 * 1024


def sample_data(size, seed=1):
    """Half-compressible data: random bytes mixed with long repeated runs."""
    rng = random.Random(seed)
    parts, total = [], 0
    while total < size:
        if rng.random() < 0.5:
            part = rng.randbytes(rng.randint(1, 5000))
        else:
            part = bytes([rng.randrange(256)]) * rng.randint(1, 20000)
        parts.append(part)
        total += len(part)
    return b"".join(parts)[:size]


class ArchiveTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def make(self, data, chunk_size=CHUNK, codec="zlib", name="video.mkv"):
        src = os.path.join(self.tmp, name)
        dst = src + ".nnr"
        with open(src, "wb") as f:
            f.write(data)
        pack(src, dst, chunk_size=chunk_size, codec=codec)
        return dst


class RoundTripTests(ArchiveTestCase):
    def test_every_codec_round_trips(self):
        data = sample_data(5 * CHUNK + 123)
        for codec in available_codecs():
            with self.subTest(codec=codec):
                path = self.make(data, codec=codec)
                with Archive(path) as archive, archive.open() as f:
                    self.assertEqual(archive.codec.name, codec)
                    self.assertEqual(f.read(), data)

    def test_sizes_around_chunk_boundaries(self):
        for size in (0, 1, CHUNK - 1, CHUNK, CHUNK + 1, 3 * CHUNK):
            with self.subTest(size=size):
                data = sample_data(size, seed=size)
                with Archive(self.make(data)) as archive, archive.open() as f:
                    self.assertEqual(archive.size, size)
                    self.assertEqual(len(archive.chunks), -(-size // CHUNK))
                    self.assertEqual(f.read(), data)

    def test_compressible_data_shrinks(self):
        data = b"frame " * 200_000
        path = self.make(data)
        self.assertLess(os.path.getsize(path), len(data) // 20)

    def test_incompressible_chunks_are_stored_raw(self):
        data = random.Random(7).randbytes(3 * CHUNK)
        path = self.make(data, codec="lzma")
        with Archive(path) as archive, archive.open() as f:
            self.assertTrue(all(c.flags & 1 for c in archive.chunks))
            self.assertLess(os.path.getsize(path), len(data) + 1024)
            self.assertEqual(f.read(), data)

    def test_pack_from_file_objects(self):
        data = sample_data(2 * CHUNK + 5)
        out = io.BytesIO()
        pack(io.BytesIO(data), out, chunk_size=CHUNK, codec="zlib", name="x.mp4")
        path = os.path.join(self.tmp, "x.nnr")
        with open(path, "wb") as f:
            f.write(out.getvalue())
        with Archive(path) as archive, archive.open() as f:
            self.assertEqual(archive.name, "x.mp4")
            self.assertEqual(f.read(), data)


class LiveExtractionTests(ArchiveTestCase):
    def test_random_seeks_match_original(self):
        data = sample_data(10 * CHUNK + 999)
        rng = random.Random(3)
        with Archive(self.make(data)) as archive, archive.open() as f:
            for _ in range(300):
                start = rng.randrange(len(data))
                length = rng.randint(0, 3 * CHUNK)
                f.seek(start)
                self.assertEqual(f.read(length), data[start : start + length])

    def test_seek_whence_and_eof(self):
        data = sample_data(3 * CHUNK)
        with Archive(self.make(data)) as archive, archive.open() as f:
            f.seek(-10, io.SEEK_END)
            self.assertEqual(f.read(), data[-10:])
            self.assertEqual(f.read(5), b"")
            f.seek(CHUNK)
            f.seek(5, io.SEEK_CUR)
            self.assertEqual(f.read(3), data[CHUNK + 5 : CHUNK + 8])
            with self.assertRaises(ValueError):
                f.seek(-1)

    def test_window_never_exceeds_safe_space(self):
        data = sample_data(20 * CHUNK)
        safe_space = 3 * CHUNK
        with Archive(self.make(data), safe_space=safe_space) as archive:
            self.assertEqual(archive.window, 3)
            with archive.open() as f:
                while f.read(4096):
                    self.assertLessEqual(len(archive._cache), archive.window)
            self.assertLessEqual(archive.stats.peak_resident, safe_space)
            self.assertEqual(archive.stats.decompressed, 20)

    def test_sequential_read_keeps_current_and_next_chunk(self):
        data = sample_data(6 * CHUNK)
        events = []
        with Archive(self.make(data), safe_space=2 * CHUNK,
                     on_event=lambda kind, i: events.append((kind, i))) as archive:
            with archive.open() as f:
                f.read(CHUNK)  # all of chunk 0; chunk 1 is being prefetched
                f.read(1)  # first byte of chunk 1
                archive._cache[2].result()  # wait for the next prefetch
                self.assertEqual(sorted(archive.resident_chunks()), [1, 2])
        self.assertIn(("evict", 0), events)

    def test_prefetch_turns_sequential_reads_into_hits(self):
        data = sample_data(8 * CHUNK)
        with Archive(self.make(data), safe_space=2 * CHUNK) as archive:
            with archive.open() as f:
                for i in range(8):
                    if i + 1 < 8:
                        f.read(CHUNK - 1)
                        archive._cache[i + 1].result()
                        f.read(1)
                    else:
                        f.read(CHUNK)
            self.assertEqual(archive.stats.misses, 1)  # only the very first chunk

    def test_window_of_one_disables_prefetch(self):
        data = sample_data(4 * CHUNK)
        with Archive(self.make(data), safe_space=CHUNK) as archive:
            self.assertEqual(archive.prefetch, 0)
            with archive.open() as f:
                self.assertEqual(f.read(), data)
            self.assertLessEqual(len(archive._cache), 1)

    def test_concurrent_readers_share_one_window(self):
        data = sample_data(12 * CHUNK)
        errors = []

        def reader(seed):
            rng = random.Random(seed)
            try:
                with archive.open() as f:
                    for _ in range(100):
                        start = rng.randrange(len(data))
                        f.seek(start)
                        if f.read(5000) != data[start : start + 5000]:
                            errors.append(seed)
            except Exception as exc:
                errors.append(exc)

        with Archive(self.make(data), safe_space=3 * CHUNK) as archive:
            threads = [threading.Thread(target=reader, args=(s,)) for s in range(8)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
        self.assertEqual(errors, [])


class IntegrityTests(ArchiveTestCase):
    def test_corrupt_chunk_is_detected(self):
        data = sample_data(3 * CHUNK)
        path = self.make(data)
        with Archive(path) as archive:
            target = archive.chunks[1]
        with open(path, "r+b") as f:
            f.seek(target.offset + target.length // 2)
            byte = f.read(1)
            f.seek(-1, io.SEEK_CUR)
            f.write(bytes([byte[0] ^ 0xFF]))
        with Archive(path) as archive, archive.open() as f:
            self.assertEqual(f.read(CHUNK), data[:CHUNK])
            with self.assertRaises(CorruptChunkError):
                f.read(10)
            # Failed chunks are not cached, and the error repeats on retry.
            with self.assertRaises(CorruptChunkError):
                f.read(10)

    def test_truncated_archive_is_rejected(self):
        path = self.make(sample_data(3 * CHUNK))
        with open(path, "r+b") as f:
            f.truncate(os.path.getsize(path) - 5)
        with self.assertRaises(FormatError):
            Archive(path)

    def test_non_archive_is_rejected(self):
        path = os.path.join(self.tmp, "plain.bin")
        with open(path, "wb") as f:
            f.write(b"x" * (HEADER.size + 100))
        with self.assertRaises(FormatError):
            Archive(path)


class ServerTests(ArchiveTestCase):
    def test_parse_range(self):
        self.assertIsNone(parse_range(None, 100))
        self.assertEqual(parse_range("bytes=0-9", 100), (0, 9))
        self.assertEqual(parse_range("bytes=90-", 100), (90, 99))
        self.assertEqual(parse_range("bytes=-10", 100), (90, 99))
        self.assertEqual(parse_range("bytes=95-500", 100), (95, 99))
        self.assertEqual(parse_range("bytes=-500", 100), (0, 99))
        for bad in ("bytes=100-", "bytes=50-10", "bytes=-0"):
            with self.assertRaises(ValueError):
                parse_range(bad, 100)

    def test_http_range_requests(self):
        data = sample_data(6 * CHUNK + 77)
        with Archive(self.make(data, name="clip.mp4"), safe_space=2 * CHUNK) as archive:
            server = ArchiveServer(archive)
            server.start_background()
            self.addCleanup(server.server_close)
            self.addCleanup(server.shutdown)

            with urllib.request.urlopen(server.url) as resp:
                self.assertEqual(resp.status, 200)
                self.assertEqual(resp.headers["Content-Type"], "video/mp4")
                self.assertEqual(resp.headers["Accept-Ranges"], "bytes")
                self.assertEqual(resp.read(), data)

            start, end = 3 * CHUNK - 10, 4 * CHUNK + 10
            req = urllib.request.Request(server.url, headers={"Range": f"bytes={start}-{end}"})
            with urllib.request.urlopen(req) as resp:
                self.assertEqual(resp.status, 206)
                self.assertEqual(resp.headers["Content-Range"],
                                 f"bytes {start}-{end}/{len(data)}")
                self.assertEqual(resp.read(), data[start : end + 1])

            req = urllib.request.Request(server.url, headers={"Range": "bytes=-100"})
            with urllib.request.urlopen(req) as resp:
                self.assertEqual(resp.read(), data[-100:])

            req = urllib.request.Request(server.url, headers={"Range": f"bytes={len(data)}-"})
            with self.assertRaises(urllib.error.HTTPError) as ctx:
                urllib.request.urlopen(req)
            self.assertEqual(ctx.exception.code, 416)

            req = urllib.request.Request(server.url, method="HEAD")
            with urllib.request.urlopen(req) as resp:
                self.assertEqual(int(resp.headers["Content-Length"]), len(data))
                self.assertEqual(resp.read(), b"")


class CliTests(ArchiveTestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, "-m", "nanoer", *args],
                              capture_output=True, check=True,
                              cwd=os.path.dirname(os.path.dirname(__file__)))

    def test_pack_info_cat_unpack(self):
        data = sample_data(4 * CHUNK + 3)
        src = os.path.join(self.tmp, "movie.avi")
        with open(src, "wb") as f:
            f.write(data)
        self.run_cli("pack", src, "--chunk-size", "64K", "--codec", "zlib")
        archive = src + ".nnr"
        info = self.run_cli("info", archive).stdout.decode()
        self.assertIn("chunks:      5", info)
        out = self.run_cli("cat", archive, "--offset", "1000", "--length", "70000").stdout
        self.assertEqual(out, data[1000:71000])
        os.remove(src)
        self.run_cli("unpack", archive)
        with open(src, "rb") as f:
            self.assertEqual(f.read(), data)


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "needs ffmpeg")
class VideoTests(ArchiveTestCase):
    def test_ffmpeg_plays_and_seeks_through_live_extraction(self):
        # Uncompressed video, the kind of input general-purpose compression
        # actually shrinks.
        video = os.path.join(self.tmp, "raw.avi")
        subprocess.run(
            ["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
             "testsrc=duration=6:size=320x240:rate=25",
             "-c:v", "rawvideo", "-pix_fmt", "yuv420p", video],
            check=True,
        )
        archive_path = video + ".nnr"
        pack(video, archive_path, chunk_size=1024 * 1024, codec="zlib")
        self.assertLess(os.path.getsize(archive_path), os.path.getsize(video) / 2)

        loaded = []
        on_event = lambda kind, i: kind == "load" and loaded.append(i)
        with Archive(archive_path, safe_space=2 * 1024 * 1024, on_event=on_event) as archive:
            server = ArchiveServer(archive)
            server.start_background()
            self.addCleanup(server.server_close)
            self.addCleanup(server.shutdown)

            probe = subprocess.run(
                ["ffprobe", "-v", "error", "-show_entries", "format=duration",
                 "-of", "csv=p=0", server.url],
                capture_output=True, check=True, text=True,
            )
            self.assertAlmostEqual(float(probe.stdout), 6.0, places=1)

            # Seek to 4s and decode one second. The player reads the header
            # (start of the file), the index (end of the file) and then only
            # the chunks around 4s; the chunks in between are never touched.
            loaded.clear()
            subprocess.run(
                ["ffmpeg", "-v", "error", "-ss", "4", "-i", server.url,
                 "-t", "1", "-f", "null", "-"],
                check=True,
            )
            self.assertLessEqual(archive.stats.peak_resident, 2 * 1024 * 1024)
            frame_chunk = int(4 / 6 * len(archive.chunks))
            skipped = set(range(4, frame_chunk - 1))
            self.assertTrue(skipped)
            self.assertFalse(skipped & set(loaded), loaded)


if __name__ == "__main__":
    unittest.main()
