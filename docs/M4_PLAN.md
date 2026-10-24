# M4 plan: space efficiency (garbage collection, dedup, small files, stats)

**Status: PROPOSED, waiting for approval.** Part of stage 1
([`STAGE1_PLAN.md`](STAGE1_PLAN.md)). No M4 code is written until this plan
is approved.

## 1. Goal

Make Nanoer keep its space over time, whatever the workload, and squeeze
out the savings that are cheap to get: duplicate data stored once, small
files compressed together. Show where the space goes with `nanoer stats`.

**Done when:**

1. **Rewrites never fill the store.** A rewrite-heavy workload (random
   overwrites of a set of files, database-like) with live data at 80 % of
   capacity runs for 10× the store's size in writes without `ENOSPC`, and
   the space used stays within 1.3× the live data once GC has caught up.
   Today, about 0.5 MB of live data can pin 16 of 23 segments (M2's
   model test) and the store fills up.
2. **GC is crash-safe.** The M3 crash-point exploration, run on workloads
   that make GC move data, finds every crash image clean and in a real
   state; `kill -9` during GC on a real mount loses nothing that was
   `fsync`ed.
3. **Duplicates are stored once.** Two identical files, or identical
   128 KiB-aligned chunks anywhere, share one stored record; deleting,
   truncating or overwriting one copy never affects the others.
4. **Small files are packed.** Files up to 16 KiB written together are
   compressed together; on `/usr`, small-file storage shrinks by about
   30 % (measured in §2).
5. **`nanoer stats`** explains the space: logical size, stored size, what
   compression, dedup, packing and holes each saved, dead space waiting
   for GC, and the RAM the metadata uses.
6. **Measured:** stored size of a copy of `/usr` against M3; a
   duplicate-heavy data set; GC write amplification; RAM per chunk;
   throughput on the M2 workloads, against M3.

**Not in M4:** recompressing cold data in GC (tiering, M5); per-folder
policies (M5); content-defined chunking (only useful for read-only images,
later); inline small files in metadata (M6, see §5); redundancy.

## 2. What the data says (measured before planning)

All of `/usr` on this machine: 77,273 files, 4,549 MiB. Each file cut into
128 KiB chunks, compressed with zstd level 1, stored raw when that does not
help, with the 64-byte record header. (Method and numbers are in
`KNOWLEDGE.md` §14.)

| | Stored | Change |
|---|---:|---:|
| ext4 (4 KiB blocks) | 4,738 MiB | |
| **Nanoer today (M3)** | **1,760 MiB = 38.7 %** of logical | |
| + dedup of identical 128 KiB chunks | 1,719 MiB | −2.3 % |
| + packing files ≤ 16 KiB into 64 KiB groups | ≈1,695 MiB | −1.4 % more |

Small files in detail:

| Files up to | Count | Logical | Stored one by one | Packed 32 K | Packed 64 K | Packed 128 K |
|---:|---:|---:|---:|---:|---:|---:|
| 2 KiB | 36,454 | 26.4 MiB | 17.2 MiB | 7.6 | 7.3 | 7.1 |
| 4 KiB | 47,470 | 57.4 MiB | 30.9 MiB | 16.7 | 15.8 | 15.3 |
| 16 KiB | 64,442 | 198.4 MiB | 82.9 MiB | 62.2 | **58.1** | 55.6 |
| 32 KiB | 70,188 | 322.9 MiB | 126.5 MiB | 105.9 | 98.1 | 93.6 |

Speed per core on incompressible (random) data: a zstd level 1 attempt
3,636 MiB/s, an LZ4 attempt 4,651 MiB/s, a sampled entropy estimate
6,860 MiB/s, BLAKE3 3,821 MiB/s.

**What this means for M4:**

- **GC is the essential part.** It does not change the ratio, but without
  it a rewrite-heavy filesystem fills up long before its data does.
- **Dedup gains little on an OS tree** (2.3 %), but costs little CPU
  (BLAKE3 is faster than compression). It pays off where data is copied:
  backups, VM images, duplicated game assets, project copies,
  `node_modules`, containers. The cost that matters is RAM (§4.2).
- **Packing pays for files up to ~16 KiB:** they shrink by 30 %. 64 KiB
  groups get nearly all of the gain of 128 KiB ones, at half the
  decompression cost per read.
- **Inlining tiny files in metadata would be worse than packing:** files
  up to 2 KiB take 26.4 MiB raw (in RAM, with M2's in-memory metadata) but
  7.3 MiB packed.
- **An incompressibility check is not worth building:** zstd already gives
  up on random data at 3.6 GB/s per core, so a pre-check could save at
  most a few percent of one core while writing media. Storing raw when
  compression does not help (M1) stays.
- **The ratio goal is already met on `/usr`** (38.7 % against the 62.5 %
  the 800 → 500 GB goal needs). M4's ratio gains are modest on an OS tree;
  the bigger lever for ratio is M5's recompression of cold data (zstd 19
  stored 37.2 % against zstd 1's 42.5 % on system files, `KNOWLEDGE.md`
  §14).

## 3. Garbage collection

**What it does.** Pick segments with little live data, copy their live
chunk records into the log, point the metadata at the copies, and let the
existing segment-release rules (`docs/format.md` §10.4) free the old
segments after the next two checkpoints.

- **Copying.** A live record's compressed payload is copied as it is
  (no decompression; recompressing cold data is M5's tiering). Its CRCs
  are checked while copying. A damaged record is left where it is (that
  segment is not freed), reported, and the errors flag is set.
- **Finding live records.** One pass over the metadata collects every
  reference into the chosen segments (a batch of several at a time); the
  segment is then read sequentially, and each data record found is kept
  if something references it. No new on-disk structure is needed.
- **Re-pointing safely.** Copies are written without holding the metadata
  lock. Then, under the lock, every reference that *still* points at the
  old location is re-pointed; a chunk overwritten in the meantime keeps
  its new data, and its copy is simply dead. The re-pointing is an
  ordinary transaction (`SetChunk` operations), appended after the copies:
  the same prefix-consistency rule as every write, so a crash at any point
  leaves either the old or the new location in use, and both are intact.
- **Choosing victims.** Cost-benefit, as in Sprite LFS: favour segments
  with little live data and old data (the segment sequence number gives
  the age), because old data tends to stay. When space is urgent, plain
  greedy (least live data first).
- **When it runs:**
  - in the background when free segments fall below a low-water mark
    (about 15 % of segments), and when the filesystem is idle and dead
    space exceeds about 25 % of what is used;
  - throttled (a time budget per pass) so foreground writes keep
    priority;
  - synchronously before any `ENOSPC`: commit, GC, two checkpoints, try
    again;
  - and offline with `nanoer gc IMAGE`, for tests and for compacting a
    filesystem before shipping an image.
- **Space for GC itself.** GC may use the reserve segments that ordinary
  writes may not, but always leaves room for the two checkpoints that free
  what it emptied. It only copies a segment when that gains space.
- **Checkpoints.** Emptied segments are freed only after two checkpoints.
  GC asks for checkpoints when it has emptied enough segments to be worth
  it. Until M6, a checkpoint rewrites all metadata, so this is batched.
- **Packs** (§5) that are mostly dead are repacked: decompressed, their
  live pieces compressed again into new packs.

## 4. Deduplication

### 4.1 How it works

- **Same grid as today.** Chunks stay fixed-size and file-aligned (128 KiB
  by default), so dedup finds identical files and identical aligned
  regions. Content-defined chunking would also find shifted data, but
  writes arrive at arbitrary offsets in a writable filesystem
  (`KNOWLEDGE.md` §6).
- **At commit,** each dirty chunk's BLAKE3 is computed on the thread pool
  *before* compression. If the dedup index has that hash, and the full
  BLAKE3 in the existing record's header matches, the file simply
  references the existing record: nothing is compressed or written.
  Duplicates within one commit are found too. BLAKE3 is collision-
  resistant, so equal hashes mean equal data (the reason it was chosen,
  `KNOWLEDGE.md` §5).
- **References.** A new operation, "share chunk", adds a reference to a
  record that is already referenced. A record's space is counted once,
  however many references it has, and is released when the last one goes.
  Counts are kept only for records with more than one reference, and are
  rebuilt from the references when a checkpoint is loaded, so there is no
  stored reference count to drift out of step.
- **GC** re-points every reference to a shared record when moving it.
- Every new data record carries its BLAKE3 (the format already has the
  field), so `fsck --deep` can check identities.

### 4.2 The index and its cost

- **In RAM:** an open-addressing table from the first 8 bytes of each
  BLAKE3 to the record's location, about 21 bytes per unique chunk.
  Collisions of the 8-byte prefix are harmless: the full hash in the
  record header is checked on every hit, and a mismatch is just a miss.
- **On disk:** each checkpoint stores the 8-byte prefixes of the hashed
  records. Records written after the checkpoint are seen by replay anyway,
  which reads their headers, so the index is complete after mount without
  reading anything extra.
- **Cost:** `/usr` has about 24,000 chunks per GiB, so 500 GB of similar
  data means about 12 million chunks: about 250 MB of RAM for the index,
  on top of M2's in-memory metadata (which M6 moves to disk). Dedup can be
  turned off with `mkfs.nanoer --no-dedup` or mount option `-o nodedup`.

## 5. Small files: packing

- **What is packed.** At commit, regular files of at most 16 KiB
  (measured, §2) written in that commit are grouped, by directory and then
  by name, into **packs** of up to 64 KiB of uncompressed data, each
  compressed as one data chunk record (flagged as a pack). Each file
  references its **piece**: the pack's location plus an offset and length
  inside it.
- **Reading** a packed file decompresses its pack (at most 64 KiB, about
  40 µs) through the safe-space cache, so reading neighbouring files
  (which is common: `grep -r`, a compiler reading headers) costs one
  decompression.
- **Changing** a packed file drops its piece; the new data is packed again
  in the next commit, or, once the file grows past 16 KiB, stored as
  ordinary chunks. A pack's space is released when its last piece goes
  (the same counting as shared records, §4.1), and GC repacks mostly dead
  packs.
- **Dedup and packing together:** a small file identical to another
  (common in `/usr`: licences, `__init__.py`, empty-ish configs) shares
  the other's piece instead of being packed again.
- **No inlining in M4.** Storing tiny files inside the metadata would cost
  RAM and checkpoint size and, measured, store worse than packing. With
  M6's on-disk metadata, inlining files of a few dozen bytes may be worth
  revisiting.

## 6. `nanoer stats`

```
nanoer stats IMAGE         an unmounted filesystem
nanoer stats MOUNTPOINT    a mounted one, through nanoerfs
```

Reports: files, logical size, stored size, and **what each technique
saved** (compression, dedup, packing, holes); stored size by codec; space
used, live, dead (what GC can reclaim) and free, by segment; GC activity
(bytes copied, segments freed, write amplification); dedup index entries;
metadata RAM.

For a mounted filesystem, `nanoerfs` answers a read-only virtual extended
attribute, `user.nanoer.stats`, on the root directory, so the command
needs no other channel to the daemon. The `user.nanoer.` prefix is
reserved for Nanoer from M4 on; M5's folder policies will use it too.

`nanoer bench` also gains dedup and packing estimates, so anyone can
measure what M4 would save on their own files before using it.

## 7. Format changes (`docs/format.md`)

| Change | Why |
|---|---|
| a new `features_incompat` bit for shared records (share-chunk operation, packs) | an M3 reader would miscount the space of a shared record; refusing to mount is right |
| new operations: share a chunk; set a packed piece | §4.1, §5 |
| data chunk flag: pack | its header owner field does not apply |
| checkpoint stream version 2: pieces (offset and length in the pack), 8-byte hash prefixes of hashed records | §4.2, §5 |
| GC: **no change** | it uses ordinary records and `SetChunk` |

New filesystems get the new bit. Existing M2/M3 filesystems keep mounting
and working, with GC, but without dedup or packing (decision 7).

`fsck` learns the new invariants: reference counts rebuilt and space
counted once per record; pieces lie inside their pack; hash prefixes match
the records' full BLAKE3; the header owner check (from M3) applies only to
records that are neither shared nor packs.

## 8. Tests

| Test | How | Where |
|---|---|---|
| **GC unit tests** | victim choice; copying and re-pointing; a file rewritten while GC copies it; a damaged record in a victim; GC inside the reserve; the `ENOSPC` path running GC | here + CI |
| **Space over time** | the rewrite workload of §1.1 at 80 % fill, 10× capacity of writes: no `ENOSPC`; space used / live data measured | here + CI (short), nightly (long) |
| **Dedup tests** | identical files and chunks share records; delete, truncate and overwrite one copy; duplicates within a commit; a forced 8-byte prefix collision never shares wrong data; dedup off | here + CI |
| **Packing tests** | many small files; read, modify, append, truncate, rename, delete; growth past 16 KiB; repacking | here + CI |
| **Model-based test** (from M2) | random operations against the reference model with dedup, packing and GC steps mixed in, plus remounts and power cuts | here + CI |
| **Crash points** (from M3) | new workloads where GC moves data, files are deduplicated and packed; every crash point nightly, a sample per push | here + CI + nightly |
| **`fsck`** | crafted damage for each new invariant: found, repaired, clean afterwards | here + CI |
| **Image fuzzing** | `fuzz_image`'s base image gains shared records and packs | here + CI + nightly |
| **Real mount** | the POSIX suite, `nanoer-fsx`, pjdfstest, xfstests fsx and `kill -9`, all on a small store so GC runs constantly | here + CI |

## 9. Files

```
include/nanoer/gc.h, src/core/gc.cpp           victim choice, copying, re-pointing
include/nanoer/dedup.h, src/core/dedup.cpp     the hash index
src/core/pack.cpp                              grouping small files into packs
include/nanoer/metadata.h, src/core/metadata.cpp   share and piece operations, counts, checkpoint v2
src/core/journal.cpp                           appending copied records
src/core/record.cpp, include/nanoer/record.h   pack flag
src/core/format.cpp, include/nanoer/format.h   feature bit
src/core/fs.cpp                                commit path (hash, lookup, packing), piece reads, GC thread, ENOSPC path
src/core/check.cpp                             new invariants
src/tools/nanoer.cpp, src/tools/mkfs.cpp       `nanoer stats`, `nanoer gc`, bench estimates; `--no-dedup`
src/fuse/nanoerfs.cpp                          `user.nanoer.stats`, `-o nodedup`
tests/unit/gc_test.cpp, dedup_test.cpp, pack_test.cpp; crash, model and check tests extended
tests/integration/                             small-store runs, rewrite workload
docs/format.md, KNOWLEDGE.md, README.md
```

## 10. Order of work

1. **Shared records:** the share operation, counting, checkpoint version
   2, the feature bit, `fsck` checks. Nothing uses them yet; existing
   tests must still pass.
2. **GC,** with its triggers, the `ENOSPC` path, `nanoer gc`, and its
   crash and space tests. The most important part, so it comes first.
3. **Dedup.**
4. **Packing.**
5. **`nanoer stats`**, bench estimates.
6. Measurements, documentation, report.

If a step shows the plan needs to change, I stop and bring it to you first.

## 11. Risks

| Risk | Mitigation |
|---|---|
| **GC moves live data, so a GC bug loses data** | copy first, re-point only what still points at the old copy, under the lock; the old copy stays intact until two checkpoints later; CRCs checked while copying; crash exploration, the model test and `fsck` after every test |
| GC needs checkpoints, and until M6 a checkpoint rewrites all metadata | GC batches its work and asks for checkpoints only when it has emptied enough; measured on a large filesystem |
| Dedup RAM: about 21 bytes per unique chunk (≈250 MB at 500 GB) on top of M2's in-memory metadata | dedup can be turned off; measured; M6 moves metadata to disk |
| A damaged shared record damages every file that shares it | `fsck` lists every affected file; redundancy is outside stage 1 (as in M3) |
| Packed small files: a read decompresses up to 64 KiB | the safe-space cache keeps the pack; measured on `grep -r` and builds |
| GC competes with foreground writes | throttled, runs when idle, urgent only when space is low |
| The ratio gains on an OS tree are small (≈4 %) | expected (§2); GC is the main deliverable; the ratio lever is M5's tiering, and stage 1's plan already says we reassess at M5 |

## 12. Decisions needed

My recommendation is listed first in each.

1. **When GC runs:** in the background when space is low or the
   filesystem is idle with much dead space, throttled, plus before any
   `ENOSPC`, plus `nanoer gc` offline (recommended); or only when space
   runs low; or only by command.
2. **GC victim choice:** cost-benefit, greedy when space is urgent
   (recommended); or greedy always (simpler, but copies hot data that
   would soon die anyway).
3. **Dedup by default:** on, with `--no-dedup` / `-o nodedup` to turn it
   off (recommended: cheap in CPU, big wins on copied data, ~2 % on an OS
   tree); off by default; or postpone dedup to M6, when the index can live
   on disk.
4. **Dedup index:** 8-byte hash prefixes in RAM and in the checkpoint,
   checked against the full BLAKE3 in the record (recommended, ≈21 bytes
   per chunk); or full 32-byte hashes (≈53 bytes per chunk, no extra read
   on a hit).
5. **Small files:** pack files up to 16 KiB into 64 KiB packs, and no
   inlining until M6 (recommended, measured in §2); or pack up to 32 KiB
   into 128 KiB packs (≈1 % smaller, twice the decompression per read).
6. **Incompressibility detection:** drop it from M4; keep storing raw
   when compression does not help (recommended: measured, the zstd attempt
   is already fast); or build an entropy pre-check anyway.
7. **Existing filesystems:** keep mounting M2/M3 filesystems, with GC but
   without dedup and packing; the new features are for new filesystems
   (recommended: there are no users yet); or add a `nanoer upgrade`
   command.
8. **`nanoer stats` on a mounted filesystem** through the virtual
   `user.nanoer.stats` attribute, reserving the `user.nanoer.` prefix
   (recommended); or for unmounted filesystems only.
