# Nanoer

**Transparent, OS-level compression for Linux.** Store more on the same disk:
with identical contents, a system that needs 800 GB without Nanoer should need
about 500 GB with it. Programs, games and the OS keep using normal files and
never notice the data is compressed.

Nanoer stores data as independently compressed chunks and decompresses only
what is read (**live extraction**), within a bounded memory budget (the **safe
space**).

## Status

**Stage 1, milestones M1 and M2 complete: Nanoer mounts.** `nanoerfs` is a
read-write compressed filesystem for Linux (through FUSE): files,
directories, hard and symbolic links, permissions, extended attributes,
`mmap`, `fsync`, sparse files. Everything survives unmounting and power cuts.
See [`docs/STAGE1_PLAN.md`](docs/STAGE1_PLAN.md) for what comes next.

**Not ready for important data yet.** Exhaustive power-cut testing and
`fsck` come in M3. Space from rewritten data is reclaimed only once a whole
segment is dead until M4 adds garbage collection, so rewrite-heavy use
(databases, VM images) can fill the store early.

| Stage | Scope |
|---|---|
| 1 | C++ engine + read-write FUSE filesystem for folders (`/home`, `/opt`, games) |
| 2 | Whole OS: compressed system image + writable layer, booted from initramfs |
| 3 | Kernel driver (C or Rust) for full speed |

## What compresses

The same 95 MiB of headers, Python library, source code and build output
takes **110 MB on ext4 and 32 MB on Nanoer** (stored at 30.5 % of its size).
On 4,158 Linux system files, the fast default (zstd level 1) stores 42.5 %
and maximum zstd 37.2 %, well below the 62.5 % that 800 GB → 500 GB needs. Already-encoded media (video, music, JPEG/PNG, most game assets)
does not compress; Nanoer stores it raw, so it costs nothing extra but saves
nothing either.

## Building

Ubuntu 24.04:

```bash
sudo apt install cmake ninja-build pkg-config g++ fuse3 \
  libzstd-dev liblz4-dev liblzma-dev libxxhash-dev libgtest-dev libfuse3-dev
cmake --preset release && cmake --build --preset release
ctest --preset release
```

Presets: `debug`, `release`, `asan` (AddressSanitizer + UBSan), `tsan`, `fuzz`
(needs Clang and `libclang-rt-18-dev`).

## Using it

```bash
mkfs.nanoer --size 20G home.img       # a sparse image file (or a block device)
mkdir -p /mnt/data
nanoerfs home.img /mnt/data           # or: mount -t nanoer home.img /mnt/data
cp -a ~/projects /mnt/data/
fusermount3 -u /mnt/data              # or: umount /mnt/data
nanoer info home.img                  # contents, logical vs stored size
nanoer bench /usr                     # measure every codec on your own files
```

`nanoerfs` options: `-o safe_space=SIZE` (decompressed-chunk cache, default
256M), `-o commit=SECONDS` (default 5), `-o ro`, `-o allow_other`, `-f`
(foreground). `mount.nanoer` is the same program, for `mount -t nanoer` and
`/etc/fstab`.

Compared with ext4 on the same disk, compiling and video playback run at the
same speed; work with many small files (untar, `find`, `grep -r`) is 2–9×
slower through FUSE. Details: `KNOWLEDGE.md` §14.

## Documentation

- [`KNOWLEDGE.md`](KNOWLEDGE.md): every technology, algorithm and design
  decision, with the reasoning and measurements.
- [`docs/STAGE1_PLAN.md`](docs/STAGE1_PLAN.md): the current plan.
- [`docs/format.md`](docs/format.md): the on-disk format specification.

## Licence

[Apache License 2.0](LICENSE).
