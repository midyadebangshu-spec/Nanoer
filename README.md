# Nanoer

**Transparent, OS-level compression for Linux.** Store more on the same disk:
with identical contents, a system that needs 800 GB without Nanoer should need
about 500 GB with it. Programs, games and the OS keep using normal files and
never notice the data is compressed.

Nanoer stores data as independently compressed chunks and decompresses only
what is read (**live extraction**), within a bounded memory budget (the **safe
space**).

## Status

**Stage 1, milestones M1–M4 complete.** `nanoerfs` is a read-write
compressed filesystem for Linux (through FUSE): files, directories, hard
and symbolic links, permissions, extended attributes, `mmap`, `fsync`,
sparse files. A power cut after any single write recovers a state that
really existed, with everything `fsync`ed (tested at every write point and
with `kill -9` on a real mount). Damage is detected and reported, never
silently dropped. Garbage collection keeps the space of rewritten data
reusable, identical data is stored once (dedup), small files are
compressed together, and `nanoer stats` shows where the space goes. See
[`docs/STAGE1_PLAN.md`](docs/STAGE1_PLAN.md) for what comes next.

**Not ready for important data yet.** Nanoer keeps one copy of
everything, so damaged data is detected but cannot be restored; keep
backups.

| Stage | Scope |
|---|---|
| 1 | C++ engine + read-write FUSE filesystem for folders (`/home`, `/opt`, games) |
| 2 | Whole OS: compressed system image + writable layer, booted from initramfs |
| 3 | Kernel driver (C or Rust) for full speed |

## What compresses

The same 95 MiB of headers, Python library, source code and build output
takes **110 MB on ext4 and 32 MB on Nanoer** (stored at 30.5 % of its size).
A copy of a whole `/usr` (4.4 GiB) is stored at 37.4 %; data with copies
(several source trees, two Python versions) 31 % smaller again thanks to
dedup.
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
nanoer fsck home.img                  # check (unmounted); --repair, --deep
nanoer stats /mnt/data                # where the space goes (or an unmounted image)
nanoer gc home.img                    # compact an unmounted image (it also runs by itself)
```

`nanoerfs` options: `-o safe_space=SIZE` (decompressed-chunk cache, default
256M), `-o commit=SECONDS` (default 5), `-o ro`, `-o nodedup`, `-o
allow_other`, `-f` (foreground). `mkfs.nanoer --no-dedup` turns dedup off
for good (it saves the index's memory, about 21–43 bytes per chunk). `mount.nanoer` is the same program, for `mount -t nanoer` and
`/etc/fstab`. `fsck.nanoer` is `nanoer fsck`, for `fsck -t nanoer` and
boot-time checks; exit codes follow e2fsck (0 clean, 1 repaired, 4 errors
left, 8 could not run).

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
