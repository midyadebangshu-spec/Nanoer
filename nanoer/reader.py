"""Live extraction: read any byte range of an archive without extracting it.

An :class:`Archive` keeps a small window of decompressed chunks in memory,
bounded by its *safe space*. Reading a byte decompresses the chunk that holds
it (unless it is already in the window) and starts decompressing the next
chunk in the background, so sequential readers such as a video player rarely
wait. When the window is full, the least recently used chunk is dropped.

    with Archive("movie.mkv.nnr", safe_space=100 * 2**20) as archive:
        with archive.open() as f:     # a normal seekable binary file object
            f.seek(1_500_000_000)
            data = f.read(4096)
"""

import io
import os
import threading
import zlib
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor

from .codecs import get_codec
from .format import (
    END_MAGIC,
    FLAG_STORED,
    HEADER,
    INDEX_ENTRY,
    MAGIC,
    TRAILER,
    VERSION,
    CorruptChunkError,
    FormatError,
)


class ChunkInfo:
    __slots__ = ("index", "offset", "length", "crc", "flags", "start", "size")

    def __init__(self, index, offset, length, crc, flags, start, size):
        self.index = index
        self.offset = offset  # where the compressed bytes live in the archive
        self.length = length  # compressed length
        self.crc = crc
        self.flags = flags
        self.start = start  # where the chunk begins in the original file
        self.size = size  # decompressed length


class Stats:
    """Counters that show what live extraction did."""

    def __init__(self):
        self.decompressed = 0  # chunks decompressed, including prefetches
        self.hits = 0  # requests answered from the window
        self.misses = 0  # requests that had to wait for a decompression
        self.evicted = 0
        self.peak_resident = 0  # most decompressed bytes held at once

    def __repr__(self):
        fields = ", ".join(f"{k}={v}" for k, v in vars(self).items())
        return f"Stats({fields})"


class Archive:
    """An open ``.nnr`` archive.

    ``safe_space`` is the most decompressed data, in bytes, kept in memory at
    once. It is rounded down to whole chunks, with a minimum of one. With room
    for two or more chunks, the next ``prefetch`` chunks are decompressed
    ahead of the reader. ``on_event(kind, chunk_index)`` is called with
    ``"load"``, ``"hit"`` or ``"evict"`` for logging.
    """

    def __init__(self, path, safe_space=None, prefetch=1, on_event=None):
        self._file = open(path, "rb")
        try:
            self._read_layout()
        except BaseException:
            self._file.close()
            raise
        self.path = path
        if safe_space is None:
            safe_space = 2 * self.chunk_size
        self.window = max(1, safe_space // self.chunk_size)
        self.prefetch = max(0, min(prefetch, self.window - 1))
        self.on_event = on_event
        self.stats = Stats()
        self._io_lock = threading.Lock()
        # Reentrant: a done-callback can run immediately, in the thread that
        # is still holding the lock inside _request.
        self._lock = threading.RLock()
        self._cache = OrderedDict()  # chunk index -> Future[bytes]
        self._pool = ThreadPoolExecutor(
            max_workers=1 + self.prefetch, thread_name_prefix="nanoer"
        )
        self._closed = False

    def _read_layout(self):
        f = self._file
        header = f.read(HEADER.size)
        if len(header) < HEADER.size:
            raise FormatError("file is too short to be a Nanoer archive")
        magic, version, codec_id, _, chunk_size, name_len = HEADER.unpack(header)
        if magic != MAGIC:
            raise FormatError("not a Nanoer archive (bad magic)")
        if version != VERSION:
            raise FormatError(f"unsupported archive version {version}")
        if chunk_size == 0:
            raise FormatError("archive has a chunk size of zero")
        self.codec = get_codec(codec_id)
        self.chunk_size = chunk_size
        self.name = os.fsdecode(f.read(name_len))

        f.seek(0, os.SEEK_END)
        self.archive_size = f.tell()
        if self.archive_size < HEADER.size + name_len + TRAILER.size:
            raise FormatError("archive is truncated")
        f.seek(-TRAILER.size, os.SEEK_END)
        index_offset, self.size, count, end_magic = TRAILER.unpack(f.read(TRAILER.size))
        if end_magic != END_MAGIC:
            raise FormatError("archive is truncated or incomplete (bad trailer)")
        if index_offset + count * INDEX_ENTRY.size + TRAILER.size != self.archive_size:
            raise FormatError("archive index does not match the file size")
        if count != -(-self.size // chunk_size):
            raise FormatError("chunk count does not match the original size")

        f.seek(index_offset)
        raw = f.read(count * INDEX_ENTRY.size)
        self.chunks = []
        for i, (offset, length, crc, flags) in enumerate(INDEX_ENTRY.iter_unpack(raw)):
            if offset + length > index_offset:
                raise FormatError(f"chunk {i} points past the end of the data")
            start = i * chunk_size
            size = min(chunk_size, self.size - start)
            self.chunks.append(ChunkInfo(i, offset, length, crc, flags, start, size))

    # -- chunk window -----------------------------------------------------

    def _decompress(self, i):
        info = self.chunks[i]
        with self._io_lock:
            self._file.seek(info.offset)
            payload = self._file.read(info.length)
        if len(payload) != info.length:
            raise CorruptChunkError(f"chunk {i} is truncated")
        try:
            data = payload if info.flags & FLAG_STORED else self.codec.decompress(payload)
        except Exception as exc:
            raise CorruptChunkError(f"chunk {i} failed to decompress: {exc}") from exc
        if len(data) != info.size or zlib.crc32(data) != info.crc:
            raise CorruptChunkError(f"chunk {i} failed its checksum")
        return data

    def _emit(self, kind, i):
        if self.on_event:
            self.on_event(kind, i)

    def _request(self, i):
        """Return the future for chunk ``i``, scheduling it if needed. Needs ``_lock``."""
        future = self._cache.get(i)
        if future is not None:
            self._cache.move_to_end(i)
            return future, True
        future = self._pool.submit(self._decompress, i)
        self._cache[i] = future
        future.add_done_callback(lambda fut, i=i: self._loaded(i, fut))
        while len(self._cache) > self.window:
            old, _ = self._cache.popitem(last=False)
            self.stats.evicted += 1
            self._emit("evict", old)
        return future, False

    def _loaded(self, i, future):
        if future.cancelled() or future.exception() is not None:
            with self._lock:
                # Drop failed chunks so the next read retries instead of
                # re-raising a stale error forever.
                if self._cache.get(i) is future:
                    del self._cache[i]
            return
        with self._lock:
            self.stats.decompressed += 1
            resident = sum(
                self.chunks[j].size
                for j, fut in self._cache.items()
                if fut.done() and not fut.cancelled()
            )
            self.stats.peak_resident = max(self.stats.peak_resident, resident)
        self._emit("load", i)

    def chunk(self, i):
        """Return the decompressed bytes of chunk ``i``."""
        if self._closed:
            raise ValueError("archive is closed")
        if not 0 <= i < len(self.chunks):
            raise IndexError(f"chunk {i} out of range")
        with self._lock:
            future, cached = self._request(i)
            ready = cached and future.done()
            if ready:
                self.stats.hits += 1
            else:
                self.stats.misses += 1
            for j in range(i + 1, min(i + 1 + self.prefetch, len(self.chunks))):
                if j not in self._cache:
                    self._request(j)
                    # Prefetching must not push the chunk being read out.
                    self._cache.move_to_end(i)
        if ready:
            self._emit("hit", i)
        return future.result()

    def resident_chunks(self):
        """Indexes of the chunks currently held decompressed, oldest first."""
        with self._lock:
            return [i for i, fut in self._cache.items() if fut.done()]

    # -- file access ------------------------------------------------------

    def open(self):
        """Return a seekable, read-only binary file object over the original data."""
        return ArchiveFile(self)

    def read_range(self, start, length):
        with self.open() as f:
            f.seek(start)
            return f.read(length)

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._pool.shutdown(wait=True, cancel_futures=True)
        with self._lock:
            self._cache.clear()
        self._file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class ArchiveFile(io.RawIOBase):
    """A file object over the original bytes of an archive.

    Several of these can share one :class:`Archive` (and its chunk window),
    each with its own position.
    """

    def __init__(self, archive):
        super().__init__()
        self.archive = archive
        self.name = archive.name
        self._pos = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self._pos

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_SET:
            pos = offset
        elif whence == io.SEEK_CUR:
            pos = self._pos + offset
        elif whence == io.SEEK_END:
            pos = self.archive.size + offset
        else:
            raise ValueError(f"invalid whence: {whence}")
        if pos < 0:
            raise ValueError("negative seek position")
        self._pos = pos
        return pos

    def readinto(self, buffer):
        if self.closed:
            raise ValueError("I/O operation on closed file")
        archive = self.archive
        view = memoryview(buffer).cast("B")
        filled = 0
        # Fill the whole buffer, crossing chunk boundaries, so read(n) only
        # comes back short at the end of the file.
        while filled < len(view) and self._pos < archive.size:
            i = self._pos // archive.chunk_size
            data = archive.chunk(i)
            start = self._pos - archive.chunks[i].start
            n = min(len(view) - filled, len(data) - start)
            view[filled : filled + n] = memoryview(data)[start : start + n]
            filled += n
            self._pos += n
        return filled

    def readall(self):
        parts = []
        while True:
            part = self.read(self.archive.chunk_size)
            if not part:
                return b"".join(parts)
            parts.append(part)
