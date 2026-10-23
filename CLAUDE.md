# CLAUDE.md

Guidance for Claude when working in this repository.

## Working rules

- **Plan first for multistep work.** Before starting any task that takes more
  than one step (new features, refactors, changes across several files, new
  tooling), write a short plan covering the approach, the files involved, the
  design choices and the open questions. Share it, discuss it, and wait for
  explicit approval before writing code. If the plan has to change partway
  through, stop and get the new plan approved too.
- Single-step requests (answering a question, a one-line fix, a typo) can go
  ahead without a plan.

## Project overview

Nanoer is a chunked compression format with **live extraction**: large files
are compressed in independent chunks and read, played or streamed without full
extraction. Only a bounded "safe space" of decompressed chunks is kept in
memory.

- `nanoer/format.py`: on-disk layout (header, chunks, index, trailer)
- `nanoer/codecs.py`: zlib / bz2 / lzma / optional zstd
- `nanoer/packer.py`: builds `.nnr` archives
- `nanoer/reader.py`: `Archive` (chunk window, prefetch) and `ArchiveFile` (seekable file object)
- `nanoer/server.py`: HTTP server with Range support for media players
- `nanoer/cli.py`: `pack`, `info`, `play`, `serve`, `cat`, `unpack`

## Commands

```bash
python -m unittest -v      # run tests (the video test needs ffmpeg)
python -m nanoer --help
```
