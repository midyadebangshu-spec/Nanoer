# M6 plan: metadata on disk, format migration, benchmark report

**Status: PROPOSED, waiting for approval.** Part of stage 1
([`STAGE1_PLAN.md`](STAGE1_PLAN.md)); the last milestone of the stage. No M6
code is written until this plan is approved.

## 1. Goal

Make Nanoer's memory use and mount time independent of how much it
stores, so that a 500 GB+ filesystem with a million files runs in a fixed,
modest amount of RAM. Let filesystems made by earlier milestones move to
the new format. Then measure the whole of stage 1 against ext4 and Btrfs
and publish the report.

**Done when:**

1. **RAM is bounded.** `nanoerfs` with 1 million files and 4 million chunks
   (≈ 500 GB of data) stays within its configured budgets (safe space +
   metadata cache + dedup index) plus at most 64 MiB, whatever the file
   count. Today: 857 MiB of metadata alone (§2).
2. **Mount and checkpoints cost what changed, not what exists.** Mounting
   that filesystem takes under 1 s plus replaying the log since the last
   checkpoint (today 12 s); a checkpoint writes the metadata that changed
   (today all 122 MiB of it, every 256 MiB of log or 5 minutes).
3. **No slower.** The M2/M4 workloads (untar, compile, `find`, `grep -r`,
   cold reads) within 10 % of M5.
4. **As safe as before.** Crash points, the model test, fsx, `kill -9` on
   real mounts and image fuzzing all run on the new format; `fsck` checks
   and repairs the on-disk metadata.
5. **Old filesystems move over.** `nanoer upgrade` converts filesystems
   made by M2–M5; contents are identical afterwards, and a crash during the
   upgrade leaves the old filesystem intact.
6. **Freed space goes back.** An image file's allocated size follows what
   Nanoer stores (today it only grows: §2).
7. **The benchmark report is published** (`docs/BENCHMARKS.md`, scripts in
   `bench/`): space, throughput, latency, CPU and RAM against ext4 and
   Btrfs with zstd, and stage 1's own conditions re-checked (≤ 62.5 % on
   OS-like data).

**Not in M6:** read-ahead of 1 MiB cold units (left open in M5), inline
small files, snapshots, multiple devices, the stage 2 OS image.

## 2. What the data says (measured before planning)

**Today's in-memory metadata at scale** (synthetic metadata applied
through the real code, release build; `KNOWLEDGE.md` §14):

| Filesystem | RAM | Checkpoint | Encode | Decode (mount) |
|---|---:|---:|---:|---:|
| 100k files × 1 chunk (13 GB) | 50 MiB | 5.8 MiB | 0.06 s | 0.27 s |
| 1M files × 1 chunk (131 GB) | 486 MiB | 59.5 MiB | 1.17 s | 5.38 s |
| 100k files × 40 chunks (524 GB) | 531 MiB | 87.3 MiB | 0.47 s | 5.05 s |
| 1M files × 4 chunks (524 GB) | 857 MiB | 122.4 MiB | 1.90 s | 12.01 s |

- RAM is about 400 bytes per file plus about 125 bytes per chunk
  (including the dedup index). At 500 GB and a million files that is most of a
  gigabyte, and it grows without limit.
- Every checkpoint rewrites everything, and GC needs two to free what it
  emptied: at this size that is a quarter of a gigabyte written for each
  round of GC, however little changed.
- Mounting decodes all metadata and rebuilds the dedup index and reference
  counts from every chunk reference: 12 s here, linear in size.

**Other findings:**
- Image files never shrink: recompressing `/usr` in M5 grew its image from
  1.7 to 3.1 GiB allocated while the data shrank. Freed segments are reused
  but never handed back to the file underneath.
- **Btrfs works on this VM** (kernel support, loop devices, `btrfs-progs`
  and `fio` installed). The stage plan expected to estimate Btrfs with a
  script; the comparison can be run for real.
- Data sets available here: `/usr` (4.4 GiB), the Linux kernel source (a
  shallow `git clone` from GitHub works; kernel.org is blocked), this
  repository, Python's libraries, and media made with `ffmpeg`.

## 3. Metadata on disk: a copy-on-write B+tree

**The tree** (`src/core/btree.cpp`). One B+tree holds all metadata, in the
style of Btrfs (KNOWLEDGE.md §7.5): items sorted by a key of (object id,
kind, offset), so everything about one file sits together.

| Kind | Object | Offset | Item |
|---|---|---|---|
| inode | inode number | 0 | the attributes |
| xattr | inode number | hash of the name | name(s) and value(s) |
| entry | directory | hash of the name | name(s) → inode (collisions share an item) |
| chunk | inode number | chunk index | the chunk reference (as today) |
| refs | record offset | 0 | reference count and **back-references** (who refers to it), for packs and shared records only |

- **Nodes** are 16 KiB, compressed with zstd 1, and written to the log as a
  new record type (decision 2). Leaves hold items; inner nodes hold keys and
  child locations.
- **Copy on write.** A change never overwrites a node: it changes a copy in
  memory (*dirty*), and the next checkpoint writes the dirty nodes, then
  their new parents up to a new root. The old nodes stay intact until two
  checkpoints later, exactly as today's records (format.md §10.4).
- **The node cache** holds nodes read from disk: its own budget
  (`-o metadata_cache=`, 64 MiB by default), 2Q like the safe space.

**What stays as it is.** Every change still goes through `apply(Op)` and is
written to the log first (the log stays the write-ahead log: commits stay
small and fast). `apply()` now changes tree items instead of in-memory
maps. A checkpoint becomes small: the dirty nodes, then a record with the
root's location, the segment table (one entry per segment, small) and the
counters (next inode, last transaction). A checkpoint is also forced once
dirty nodes reach a limit (32 MiB), which bounds memory between them.

**Mounting** reads the checkpoint record and replays the log after it into
the tree (the replayed changes are dirty nodes, written at the next
checkpoint). Nothing proportional to the filesystem's size is read.

**What has to change because nothing is scanned at mount any more:**

| Today | With the tree |
|---|---|
| reference counts rebuilt at mount from every reference | stored in `refs` items |
| dedup index rebuilt at mount from every reference | rebuilt in the background after mount, in key order, at low priority (decision 4); duplicates are missed until it is done |
| GC finds a victim's users by scanning every file | a record's header names its one owner; packs and shared records have back-references (decision 5) |
| the tiering pass and `nanoer stats` scan every inode in memory | they walk the tree in key order (sequential node reads), with a cursor between steps |
| mount cross-checks space accounting against every reference | `fsck` does it (it reads everything anyway) |

**Directory listings** come out in name-hash order, which gives each entry
a stable position (good for `telldir`/`seekdir` and NFS-style cookies); `ls`
sorts anyway.

## 4. Old filesystems: `nanoer upgrade`

Filesystems made by M2–M5 store metadata as full checkpoints. The new code
keeps today's loader for reading them, and:

- **mounting one read-write is refused**, with a message pointing to
  `nanoer upgrade` (decision 6); read-only mounts and `fsck` still work;
- `nanoer upgrade IMAGE` loads the metadata, writes it as a tree into free
  segments, and switches over by writing the superblock (the same atomic
  step as every checkpoint). A crash before that leaves the old filesystem
  as it was; after it, the new one is complete. It also turns on
  `shared_records` and `cold_data` for filesystems that predate them (M5
  left those without cold data).
- It needs free space for one copy of the metadata (checked first).

At the end of M6 the format is declared **1.0** (no longer a draft;
decision 7): later changes are new feature bits.

## 5. Giving space back (discard)

When a segment becomes free (two checkpoints after it emptied), Nanoer
tells the layer below: a hole punched in an image file
(`FALLOC_FL_PUNCH_HOLE`), a discard on a block device (`BLKDISCARD`), so
SSDs and thin storage reclaim it too (decision 8). In the background, a
batch at a time, after the superblock that frees it is durable. A segment
being reused is simply written again. `-o nodiscard` turns it off.

## 6. The benchmark report

`bench/` gets scripts that anyone can rerun; `docs/BENCHMARKS.md` the
results and conclusions, on this VM (decision 9).

- **Filesystems:** ext4 (baseline), Btrfs `compress=zstd:1`, Btrfs
  `compress-force=zstd:3`, Nanoer (default policy, then cold), all on loop
  devices or image files on the same disk.
- **Data sets:** `/usr`; the Linux kernel source; a mixed home directory
  (code, documents, Python libraries, media from `ffmpeg`, copies); a media
  set (to show what does not compress).
- **Space:** stored size, image allocated size, 62.5 % target per data set.
- **Speed:** `fio` sequential and random reads and writes, at several block
  sizes and queue depths; real workloads (copy in, untar, compile, `find`,
  `grep -r`, `git status`, delete), warm and cold; latency percentiles.
- **Cost:** CPU time and RAM (`nanoerfs` RSS; for Btrfs, kernel CPU time).
- **Honesty rules:** median of several runs with the spread; caches
  dropped for cold runs; the same order for every filesystem; noted when the
  VM's noise exceeds a difference.

## 7. Format changes (`docs/format.md`)

| Change | Why |
|---|---|
| incompat feature `metadata_tree` | older readers cannot read tree metadata |
| record type 5: tree node (header, level, keys, items or child pointers) | the metadata on disk |
| checkpoint version 3: root location, segment table, counters | a checkpoint no longer holds all metadata |
| item encodings for the five kinds; `refs` items with back-references | so another implementation (stage 3) can read them |
| version 1.0 final | the format stops being a draft |

Discard needs no format change.

## 8. Tests

| Test | How | Where |
|---|---|---|
| **Tree unit tests** | insert, update, delete, range scans, splits and merges at every level, keys colliding by hash, CoW leaves the old tree readable | here + CI |
| **Tree against a model** | random operations on the tree and on a `std::map`, compared after each; checkpoints and reloads in between | here + CI |
| **Metadata against M5's** | every operation applied to the tree-backed metadata and to today's in-memory metadata (kept as a test oracle), compared | here + CI |
| **Crash points** (from M3) | the existing workloads on the new format, with checkpoints small enough to happen often; every crash point nightly | here + CI + nightly |
| **Model test, fsx, POSIX suite, `kill -9`** | unchanged, on the new format | here + CI + nightly |
| **`fsck`** | tree damage: a node unreadable, keys out of order, a wrong level, a dangling child, wrong counts or back-references; found, reported, repaired | here + CI |
| **Image fuzzing** | the base image becomes a tree; node parsing fuzzed | here + CI + nightly |
| **Upgrade** | images made by M2, M3, M4 and M5 builds (kept as test images), upgraded, compared, `fsck` clean; a crash at every write point of an upgrade | here + CI |
| **Discard** | freed segments are punched; reused ones written again; a crash between freeing and punching | here + CI |
| **Scale** | 1M files and 4M chunks: RAM, mount time, checkpoint size and time | here |

## 9. Files

```
include/nanoer/btree.h, src/core/btree.cpp       the CoW B+tree, node cache, nodes on disk
include/nanoer/metadata.h, src/core/metadata.cpp apply() on tree items; refs and back-references
src/core/journal.cpp                             checkpoint v3, replay into the tree, dirty-node limit
src/core/fs.cpp, gc.cpp, tier.cpp, stats.cpp     key scans instead of in-memory maps
src/core/dedup.cpp                               background rebuild, bounded
src/core/check.cpp                               tree checks and repairs
src/core/upgrade.cpp, src/tools/nanoer.cpp       nanoer upgrade
src/core/io.cpp                                  discard (punch holes, BLKDISCARD)
src/fuse/nanoerfs.cpp                            -o metadata_cache=, -o nodiscard
tests/unit/btree_test.cpp, upgrade_test.cpp, discard_test.cpp; existing tests on the new format
tests/images/                                    small images made by M2–M5, for upgrade tests
bench/                                           benchmark scripts
docs/format.md, docs/BENCHMARKS.md, KNOWLEDGE.md, README.md
```

## 10. Order of work

1. **The tree alone:** nodes, CoW, cache, scans, with its own tests and
   the model test. Node size and compression measured before fixing them.
2. **Metadata on the tree:** `apply()`, checkpoints, replay, reference
   counts and back-references; GC, tiering, stats and dedup on key scans;
   the M5 metadata as a test oracle.
3. **Safety:** crash points, `fsck` for the tree, fuzzing; the format
   document.
4. **Upgrade and discard.**
5. **Scale and speed:** the 1M-file measurements, the workloads against M5.
6. **The benchmark report.**
7. Documentation and the report to you.

Steps 1–3 are the largest and riskiest part of stage 1; I report after
step 3, before upgrade and benchmarks. If a step shows the plan needs to
change, I stop and bring it to you first.

## 11. Risks

| Risk | Mitigation |
|---|---|
| A tree bug loses or corrupts metadata | CoW keeps the previous tree intact until two checkpoints later; the model and oracle tests compare every operation; crash points on the new format; `fsck` walks the whole tree |
| Small-file workloads get slower (a lookup may now read a node) | the node cache; measured against M5 (§1.3); a related file's items share leaves |
| Dedup finds fewer duplicates right after mount | the index is a hint; rebuilt in the background; measured (how long, on 4M chunks) |
| Back-references cost space and writes for shared data | only for packs and shared records, which are a minority (§14: 2,082 shared records in `/usr`); measured |
| `fsck` of a huge filesystem needs much memory | it streams the tree in key order; its counts are per record, as now; measured at 1M files |
| Upgrade needs room for a second copy of the metadata | checked before starting, with the amount stated |
| Benchmarks on a noisy VM | medians and spreads, several runs, differences below the noise called so |
| M6 is large | the order above keeps a working filesystem at every step; the report after step 3 |

## 12. Decisions needed

My recommendation is listed first in each.

1. **Tree shape:** one tree with (object, kind, offset) keys, like Btrfs
   (recommended: a file's metadata together, one structure to make
   crash-safe); or a tree per kind (inodes, entries, chunks).
2. **Nodes:** 16 KiB, compressed with zstd 1 (recommended: metadata
   compresses well and fewer, larger nodes mean shallower trees; confirmed
   by measurement in step 1); or 4 KiB uncompressed.
3. **Directory entries:** keyed by a 64-bit hash of the name, collisions
   sharing an item (recommended: fixed-size keys, stable listing positions);
   or by the full name (variable-length keys, listings in name order).
4. **Dedup index:** kept in RAM within a budget (128 MiB by default),
   rebuilt in the background after mount (recommended: mount stays fast,
   and 4M unique chunks need about 84–172 MiB); or stored in the tree (one
   random lookup per written chunk); or unbounded in RAM as today.
5. **Finding a record's users (GC):** back-references for packs and
   shared records only, the header's owner for the rest (recommended); or
   scanning the tree for every GC step.
6. **Filesystems from M2–M5:** `nanoer upgrade` required before writing;
   read-only mounts and `fsck` still work (recommended); or keep both
   metadata engines writable.
7. **Format:** declared 1.0 at the end of M6 (recommended); or keep it a
   draft until stage 2.
8. **Discard:** on by default, in the background (recommended); off by
   default (`-o discard`); or not in M6.
9. **Benchmarks:** run on this VM against ext4, Btrfs `zstd:1` and
   `compress-force=zstd:3`, with `/usr`, the Linux kernel source, a mixed
   home directory and a media set, plus scripts to rerun anywhere
   (recommended); or a smaller set.
10. **Metadata cache:** 64 MiB by default, `-o metadata_cache=`
    (recommended); or a share of the safe space.
