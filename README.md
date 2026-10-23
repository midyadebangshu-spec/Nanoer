# Nanoer: live extraction

Compress a huge file into independent chunks, then **use it without ever
extracting it**. A player reading a 2 GB video through Nanoer holds only a
small, fixed *safe space* of decompressed data. It decompresses the chunk it
is playing, prepares the next one in the background, and drops the old one.

```
 archive on disk (compressed)                     memory: safe space = 2 chunks
┌────┬────┬────┬────┬────┬────┬─────┐
│ c0 │ c1 │ c2 │ c3 │ c4 │ c5 │ ... │  ──►   [ c2: playing ] [ c3: prefetched ]
└────┴────┴────┴────┴────┴────┴─────┘          c1 evicted, c4 next
```

## Quick start

```bash
pip install .                      # no dependencies (optional: pip install .[zstd])

nanoer pack movie.mkv --chunk-size 50M          # -> movie.mkv.nnr
nanoer info movie.mkv.nnr
nanoer play movie.mkv.nnr --safe-space 100M     # opens mpv / vlc / ffplay / browser
```

The 2 GB / 50 MB example: `--chunk-size 50M` at pack time and `--safe-space
100M` at play time give one 50 MB chunk playing and one being prepared. With
`--safe-space 50M` the window is a single chunk and there is no prefetch, so
playback pauses briefly at every chunk boundary.

Other commands:

| command | what it does |
|---|---|
| `nanoer serve ARCHIVE` | stream over HTTP (`http://127.0.0.1:8642/<name>`) for any player or browser, with seeking |
| `nanoer cat ARCHIVE [--offset N --length N]` | write the original bytes to stdout, e.g. `nanoer cat x.nnr \| mpv -` |
| `nanoer unpack ARCHIVE` | ordinary full extraction |
| add `-v` to `play`/`serve`/`cat` | log every chunk load, hit and eviction |

### From Python

```python
from nanoer import Archive, pack

pack("movie.mkv", "movie.mkv.nnr", chunk_size=50 * 2**20)

with Archive("movie.mkv.nnr", safe_space=100 * 2**20) as archive:
    with archive.open() as f:          # a normal seekable binary file object
        f.seek(1_500_000_000)
        frame_data = f.read(65536)     # decompresses only the chunk(s) needed
    print(archive.stats)
```

## How it works

* **Format** (`nanoer/format.py`): a header, chunks compressed independently
  (zlib, bz2, lzma or zstd), then an index of chunk offsets and CRC-32
  checksums. Any byte offset maps straight to a chunk: `offset // chunk_size`.
* **Window** (`nanoer/reader.py`): an LRU cache of decompressed chunks capped
  at `safe_space // chunk_size` chunks. Reads that cross chunk boundaries are
  stitched together. Background threads prefetch the next chunk. Several
  readers can share one window safely.
* **Streaming** (`nanoer/server.py`): a local HTTP server that answers
  `Range` requests. This is how real players seek, so mpv, VLC, ffmpeg and
  browsers play the archive as if it were the original file.
* **Integrity:** every chunk is checked against its CRC. Truncated or corrupt
  archives raise `FormatError` / `CorruptChunkError` and never return wrong
  bytes.

## Important: what compresses

General-purpose compression **cannot shrink an already-encoded video**. MP4,
MKV and WebM files using H.264, H.265, VP9 or AV1 are already compressed
close to their limit. A measured example from this repo's tests:

| input | codec | result |
|---|---|---|
| H.264 MP4, 9.6 MiB | zlib / bz2 / lzma | **99.8–99.9 %** of the original (9 of 10 chunks stored raw) |
| uncompressed AVI (synthetic test pattern), 16.5 MiB | lzma | 0.8 % |

So 2 GB → 500 MB is realistic for **raw or lightly compressed** data, such as
uncompressed or intra-frame camera footage, screen recordings, disk images,
logs, datasets and game assets. To shrink a normal H.264 movie, re-encode it
(for example `ffmpeg -i in.mp4 -c:v libx265 -crf 26 out.mkv`). Live extraction
still applies to anything you pack: chunks that do not compress are stored raw
automatically, so the archive is never meaningfully larger than the original.

## Trade-offs

* **Seek latency ≈ time to decompress one chunk.** Smaller chunks mean faster
  seeks but a slightly worse ratio. On one core, lzma decompresses roughly
  50–100 MB/s and zstd several hundred MB/s. The default chunk size is 16 MiB.
* **Container metadata.** Players read the file header and often an index at
  the end of the file. With a window of only two chunks those get re-fetched
  now and then, so give the window a little extra room (3–4 chunks) for
  smoother seeking.
* Decompressed data lives in RAM, not on disk, so nothing is left behind and
  the disk does not wear.

Related ideas this builds on: the zstd seekable format, BGZF (bgzip), xz
multi-block files, squashfs, and HLS/DASH segmenting.

## Tests

```bash
python -m unittest -v        # the video test runs when ffmpeg is installed
```
