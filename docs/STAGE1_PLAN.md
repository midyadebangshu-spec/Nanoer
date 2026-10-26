# Stage 1 plan: C++ engine and read-write FUSE filesystem

**Status: APPROVED 2026-10-24** with the recommended answers to every
question in §9. **M1, M2 and M3 complete** (2026-10-24); see
[`M2_PLAN.md`](M2_PLAN.md) and [`M3_PLAN.md`](M3_PLAN.md); **M4 complete**
(2026-10-26), see [`M4_PLAN.md`](M4_PLAN.md). Background and reasoning for each technology are in
[`KNOWLEDGE.md`](../KNOWLEDGE.md).

## 1. Goal

A working **read-write compressed filesystem for Linux**, `nanoerfs`. You
create a Nanoer store, mount it on a folder such as `/home`, `/opt` or a games
directory, and every program uses it normally while data is stored
compressed.

**Done when:**

1. It passes POSIX conformance (pjdfstest) and data-integrity (fsx) tests.
2. It survives simulated power cuts at every write point, losing no data that
   was `fsync`ed.
3. Benchmarks against **ext4** and **Btrfs + zstd** have been run and reported:
   space used, throughput, latency, CPU time.
4. On OS-like data (system files, apps, code), it stores at most **62.5 %** of
   the original size (the 800 → 500 GB target).

**Not in stage 1:** booting the OS from Nanoer (stage 2), the kernel driver
(stage 3), Windows/macOS, encryption, snapshots exposed to users (the design
keeps them possible), multi-disk setups.

## 2. Architecture

```
 any program
     │ read/write/mmap
 ┌───▼──────────────── Linux kernel ─────────────────┐
 │ VFS ── page cache ── FUSE module ── /dev/fuse     │
 └─────────────────────────────────────┬─────────────┘
                                       │
 ┌─ nanoerfs (user space, C++20) ──────▼─────────────┐
 │ FUSE adapter (libfuse3 low-level API)              │
 │   │                                                │
 │ Filesystem layer: inodes, directories, links,      │
 │   xattrs, permissions, rename, truncate, fsync     │
 │   │                                                │
 │ Data layer: file → chunks, write buffers,          │
 │   safe-space cache, read-ahead, policies           │
 │   │                                                │
 │ Storage layer: segments (append-only log),         │
 │   metadata index, checkpoints, GC/tiering, dedup   │
 │   │                                                │
 │ Primitives: codecs, checksums, I/O backend         │
 └───────────────────┬────────────────────────────────┘
                     │
         backing store: one image file on any filesystem,
         or a raw block device / partition
```

**Backing store (proposed):** a single **image file or block device**. An
image file means no reformatting: the store lives on your existing ext4
filesystem (`/var/nanoer/home.img`). A block device is what stage 2/3 need.
Both use the same format.

## 3. On-disk format (draft)

The full byte-level specification goes in `docs/format.md` (milestone 1), so a
future C or Rust kernel driver can implement it independently. All integers
are little-endian.

```
offset 0 ┌──────────────────────────────┐
         │ superblock A (4 KiB)         │  magic, format version, UUID, block size,
         │ superblock B (4 KiB)         │  segment size, latest checkpoint location,
         ├──────────────────────────────┤  generation number, CRC-32C
         │ segment 0  (e.g. 64 MiB)     │
         │ segment 1                    │  segments hold records, appended in order:
         │ ...                          │   • data chunk (compressed, XXH3)
         │                              │   • metadata log entry (CRC-32C)
         │                              │   • checkpoint (metadata snapshot)
         └──────────────────────────────┘
```

- **Two superblocks**, written alternately. Each has a generation number and a
  checksum. Mount picks the newest valid one, so a torn superblock write
  cannot break the filesystem.
- **Segments:** fixed-size, append-only. A segment summary records what each
  record is, so GC and `fsck` can scan without the index.
- **Data chunk record:** header (codec id, flags, uncompressed length,
  compressed length, XXH3-64 of the uncompressed data, BLAKE3 id when deduped)
  followed by the payload. Codec "none" means stored raw.
- **Metadata:** inode records, directory entries, the file extent map
  (inode, offset) → chunk location, the dedup index (BLAKE3 → chunk + refcount)
  and the segment usage table.
  - **M2:** held in memory, persisted as a log of changes plus periodic
    checkpoints. Simple, and enough to make the filesystem correct.
  - **M6:** moved into an on-disk **CoW B+tree**, so RAM use no longer grows
    with the filesystem size. This is needed for 500 GB+ stores.
- **Format versioning:** a version number plus feature flags in the
  superblock. Unknown required features refuse to mount; unknown optional
  features mount read-only.

## 4. How the main operations work

**Read:** look up chunk(s) for (inode, offset) → safe-space cache hit? return
the bytes : read the record, decompress, verify XXH3, insert into the cache →
the reply goes to the kernel → the page cache. Sequential access triggers
background prefetch of the following chunks.

**Write:** the kernel writeback cache batches writes → nanoerfs collects dirty
data per file into 128 KiB chunk buffers → on buffer full, `fsync`, close or a
~5 s timer: incompressibility check → compress (zstd level 1 by default) on
the thread pool → append to the open segment → log the extent-map update. The
old chunk's space becomes dead and is reclaimed by GC.

**fsync:** flush the data records → `fdatasync` → write the commit record →
`fdatasync` → reply. Only then is the data guaranteed after a power cut.

**Crash recovery (mount):** pick the newest valid superblock → load the last
checkpoint → replay log entries after it, stopping at the first invalid or
incomplete one → mount.

**GC + tiering:** a background thread picks segments with a low live fraction
→ copies live chunks to a new segment, **recompressing cold chunks** with zstd
level 19 (or LZMA, by policy) → frees the old segment. Runs when idle,
throttled.

**Dedup:** a chunk's BLAKE3 hash is looked up before writing. On a hit,
increment the refcount and point to the existing chunk instead of writing.

**Small files:** files ≤ ~2 KiB are stored inline in metadata. Small files
written together are packed into shared chunks.

**Policies:** per-folder settings through an xattr (`user.nanoer.policy`),
e.g. `fast` (LZ4), `balanced` (zstd, the default), `max` (cold data to LZMA),
`none`. Inherited by new files.

## 5. Repository layout

```
CMakeLists.txt, CMakePresets.json     build, plus debug/release/sanitizer presets
docs/format.md                        on-disk format specification
include/nanoer/                       public headers (C++ engine API)
src/core/
  codec.{h,cpp}                       LZ4, zstd, LZMA, none behind one interface
  checksum.{h,cpp}                    CRC-32C, XXH3, BLAKE3 wrappers
  io.{h,cpp}                          backing store: image file / block device, plus a fault-injecting test store
  superblock.{h,cpp}, segment.{h,cpp}, record.{h,cpp}
  metadata.{h,cpp}, checkpoint.{h,cpp}, log.{h,cpp}
  btree.{h,cpp}                       (M6)
  cache.{h,cpp}                       safe space: sharded LRU, later 2Q/ARC
  chunker.{h,cpp}, dedup.{h,cpp}, policy.{h,cpp}
  gc.{h,cpp}                          GC + tiering
  fs.{h,cpp}                          inode/directory/file semantics
src/fuse/nanoerfs.cpp                 libfuse3 low-level adapter, mount options
src/tools/
  mkfs_nanoer.cpp                     create a store
  nanoer.cpp                          `nanoer stats | fsck | gc | policy | bench`
tests/unit/                           GoogleTest
tests/integration/                    mount-based tests, pjdfstest/fsx runners
tests/crash/                          power-cut simulation
fuzz/                                 libFuzzer targets
bench/                                benchmark scripts and data sets
.github/workflows/ci.yml              GCC + Clang, sanitizers, tests
```

## 6. Milestones

Each milestone ends with a **report to you** (what works, test results,
numbers). The next milestone starts only after you approve it. Changes to this
plan come back to you first.

| # | Milestone | Deliverables | Verified by |
|---|---|---|---|
| **M1** ✅ | Foundation | CMake build, CI, codecs, checksums, I/O backend, superblock, segments, records, `docs/format.md`, `mkfs.nanoer` | unit tests, codec round-trip fuzzing, sanitizers in CI |
| **M2** ✅ | Working filesystem | metadata (in memory + log + checkpoint), read/write paths, safe-space cache, FUSE adapter: files, directories, symlinks, hard links, permissions, xattrs, rename, truncate, sparse files, mmap, fsync | pjdfstest, fsx, model-based tests, manual mount |
| **M3** ✅ | Crash safety | strict write ordering, recovery/replay, `nanoer fsck` | power-cut simulation at every write point; corrupted-image fuzzing |
| **M4** ✅ | Space efficiency | GC/compaction, dedup with refcounts, packed small files (inlining measured worse and dropped; incompressibility detection measured not worth it), `nanoer stats` | GC/dedup unit + crash tests; ratio measured on a copy of `/usr` |
| **M5** | Smart features | hot/cold tiering in GC, per-folder policies, read-ahead, 2Q/ARC if benchmarks need it | tiering tests; sequential-read benchmarks |
| **M6** | Scale + benchmarks | on-disk CoW B+tree metadata, format migration, full benchmark report against ext4 and Btrfs + zstd | crash + fsx tests again; RAM use at large file counts; benchmark report |

## 7. Benchmarks

- **Data sets:** a copy of `/usr` (OS files), a source tree (e.g. the Linux
  kernel), a small game or asset collection, a mixed home directory with
  media.
- **Measurements:** stored size (the 800 → 500 GB check), sequential and
  random read/write throughput (fio), latency, CPU usage, RAM, real workloads
  (untar, compile, `find`/`grep` over a tree).
- **Comparisons:** ext4 (baseline) and Btrfs mounted `compress=zstd:1` and
  `compress-force=zstd:3`.
- **Limitation:** this container may not allow mounting Btrfs (it needs loop
  devices and privileges). I'll measure Btrfs's *compression ratio* by
  replaying its 128 KiB-extent method in a script, and give you a script to
  run the full comparison on your machine.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Data loss bugs | CoW design, checksums everywhere, crash simulation, fsx, fuzzing; do not use for irreplaceable data until M3+ |
| FUSE overhead makes it slow | writeback cache, large requests, multi-threaded loop, LZ4 policy; the stage 3 kernel driver is the real fix |
| Ratio no better than Btrfs | tiering + dedup + small-file packing are the levers; measured at M4/M5, and we reassess together if they fall short |
| Scope is large | strict milestones, each reviewed before the next |
| Environment limits (no Btrfs mount, no boot here) | scripts you can run locally; CI where possible |

## 9. Decisions (all approved as recommended, 2026-10-24)

1. **Backing store:** image file **and** block device, sharing one format
   (recommended), or only one of them?
2. **Metadata in memory first (M2), on-disk B+tree later (M6):** okay? It gets
   a correct filesystem working sooner; the alternative is building the
   B+tree first, which is slower to a first working mount.
3. **Dedup in stage 1 (M4):** keep it, or postpone to focus on compression?
4. **Default chunk size 128 KiB** and **default codec zstd level 1** for new
   writes: okay?
5. **CI on GitHub Actions** (free for public repositories, limited minutes for
   private ones): okay?
6. **Dependencies from Ubuntu packages** (`libfuse3-dev`, `libzstd-dev`,
   `liblz4-dev`, `liblzma-dev`, `libxxhash-dev`) with BLAKE3 vendored as
   source: okay?
