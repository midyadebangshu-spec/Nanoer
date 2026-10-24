# Nanoer

**Transparent, OS-level compression for Linux.** Store more on the same disk:
with identical contents, a system that needs 800 GB without Nanoer should need
about 500 GB with it. Programs, games and the OS keep using normal files and
never notice the data is compressed.

Nanoer stores data as independently compressed chunks and decompresses only
what is read (**live extraction**), within a bounded memory budget (the **safe
space**).

## Status

**Stage 1, milestone M1 (foundation) complete.** Stage 1 builds a C++20
engine and a read-write FUSE filesystem (`nanoerfs`); see
[`docs/STAGE1_PLAN.md`](docs/STAGE1_PLAN.md). M1 delivered the codecs,
checksums, storage back ends, the on-disk format
([`docs/format.md`](docs/format.md)) and the first tools. Nothing can be
mounted yet; that is milestone M2.

| Stage | Scope |
|---|---|
| 1 | C++ engine + read-write FUSE filesystem for folders (`/home`, `/opt`, games) |
| 2 | Whole OS: compressed system image + writable layer, booted from initramfs |
| 3 | Kernel driver (C or Rust) for full speed |

## What compresses

Measured on 4,158 Linux system files: stored at **42.5 %** of their size
with the fast default (zstd level 1) and **37.2 %** with maximum zstd, well
below the 62.5 % that 800 GB → 500 GB needs. Already-encoded media (video, music, JPEG/PNG, most game assets)
does not compress; Nanoer stores it raw, so it costs nothing extra but saves
nothing either.

## Building

Ubuntu 24.04:

```bash
sudo apt install cmake ninja-build pkg-config g++ \
  libzstd-dev liblz4-dev liblzma-dev libxxhash-dev libgtest-dev libfuse3-dev
cmake --preset release && cmake --build --preset release
ctest --preset release
```

Presets: `debug`, `release`, `asan` (AddressSanitizer + UBSan), `tsan`, `fuzz`
(needs Clang and `libclang-rt-18-dev`).

## Tools so far

```bash
mkfs.nanoer --size 20G home.img           # new sparse image file (or a block device)
nanoer info home.img                      # superblocks, geometry, settings
nanoer bench /usr                         # measure every codec on your own files
```

## Documentation

- [`KNOWLEDGE.md`](KNOWLEDGE.md): every technology, algorithm and design
  decision, with the reasoning and measurements.
- [`docs/STAGE1_PLAN.md`](docs/STAGE1_PLAN.md): the current plan.
- [`docs/format.md`](docs/format.md): the on-disk format specification.

## Licence

[Apache License 2.0](LICENSE).
