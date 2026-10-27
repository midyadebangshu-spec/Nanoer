# M5 plan: smart features (cold data recompressed, folder policies)

**Status: PROPOSED, waiting for approval.** Part of stage 1
([`STAGE1_PLAN.md`](STAGE1_PLAN.md)). No M5 code is written until this plan
is approved.

## 1. Goal

Store data that has stopped changing in its smallest practical form,
automatically and without slowing down reads, and let users say what
matters for each folder (speed, size, or no compression at all).

**Done when:**

1. **Cold data is recompressed by itself.** Data that has not been written
   for a while is rewritten in the background in a denser form; a copy of
   `/usr` left to go cold takes at least 10 % less space than today
   (37.4 % of its size now; about 32–33 % expected, §2).
2. **Reading it is as fast as before.** Sequential reads of cold data within
   10 % of hot data on a real mount; the cost of a random 4 KiB read of cold
   data measured and reported.
3. **It is crash-safe.** The crash-point exploration, run on workloads where
   data is recompressed, recovers a real state at every point; `kill -9`
   during recompression loses nothing that was `fsync`ed.
4. **Folder policies work:** `none`, `fast`, `balanced` (default) and `max`
   on a directory decide how new files under it are compressed and how
   their cold data is stored; new files and folders inherit them.
5. **The cache decision is made by measurement:** a scan benchmark shows
   whether a scan-resistant cache (2Q) is worth it, and it is built only if
   it is (§6).
6. **Measured:** `/usr` and the M4 data sets before and after going cold;
   recompression speed and write amplification; read speeds, hot and cold.

**Not in M5:** a read-ahead of Nanoer's own (measured not to be the
bottleneck, §2; decision 4), recompressing to LZMA automatically, on-disk
metadata (M6), anything Windows.

## 2. What the data says (measured before planning)

**Codec, level and chunk size**, on 512 MB of `/usr` (22,966 files), one
thread (`nanoer bench`):

| Codec | 128 KiB chunks: stored | 1 MiB units: stored | Compress MB/s (1 MiB) | Decompress MB/s (1 MiB) |
|---|---:|---:|---:|---:|
| zstd 1 (**today**, 128 KiB) | **41.1 %** | 40.1 % | 270 | 1,176 |
| zstd 3 | 39.6 % | 37.2 % | 113 | 1,047 |
| **zstd 9** | 37.6 % | **35.3 %** | **43** | **1,175** |
| zstd 19 | 35.9 % | 33.2 % | 3 | 873 |
| LZMA 6 | 34.5 % | 31.8 % | 3 | 45 |

- **zstd 9 in 1 MiB units** stores 14 % less than today and decompresses
  as fast. At 43 MB/s per core, recompressing 500 GB takes about an hour on
  three cores.
- **zstd 19** gains 2 points more but compresses at 3 MB/s per core: 500 GB
  would take two days on three cores. It fits the `max` policy, not the
  default.
- **LZMA** reads 25× slower: never automatic.

**Reading**, measured on this VM:

| | |
|---|---:|
| the engine alone (no FUSE), sequential, compressible data, one thread | ≈ 2,000 MB/s |
| through a cold mount (`dd` to `/dev/null`), compressible log data | 808 MB/s |
| through a cold mount, incompressible data (stored raw) | 468 MB/s |
| zstd 1 decompression of that log data alone | 4,643 MB/s |

Decompression is not what limits streaming reads; FUSE is (its per-request
round trips, which the stage 3 kernel driver removes). A read-ahead that
decompresses chunks in parallel before they are asked for would have little
to speed up.

## 3. Cold data recompressed (tiering)

**What is cold.** A regular file not written for `cold_after` (7 days by
default; `-o cold_after=` to change), whatever its reads (decision 1).
Since zstd 9 decompresses as fast as zstd 1, recompressing data that is
still read often costs those reads nothing; it would be wasted only on data
that is about to change, and that is what the write age excludes. Reads
never write to the disk for this (no access-time updates, as decided in
M2).

**What it becomes** (decision 2): the file's chunks, eight consecutive
128 KiB chunks at a time, compressed together with zstd 9 into one 1 MiB
**cold unit**. A cold unit is a pack (the M4 mechanism) whose pieces are
one file's consecutive chunks, so the read path, reference counting, GC and
`fsck` already handle it. A file's last few chunks, or files of fewer than
eight chunks, form shorter units. Small files already in packs are packed
again with zstd 9.

**How.** A background pass, like GC and sharing its machinery:

1. under the shared lock, find cold files whose chunks are not yet cold
   (each chunk reference carries a `cold` flag once recompressed, so
   nothing is examined twice);
2. with no lock, read and decompress their chunks and compress the cold
   units (the slow part, on the thread pool);
3. under the exclusive lock, append the units and re-point only the chunks
   that still refer to what was read, then commit. A file written in the
   meantime keeps its new data.

It runs when the filesystem is idle and has free space to spare (it writes
the new form before the old one is freed), throttled, and pauses when
writes resume. GC, when it moves the chunks of a cold file that are not
yet cold, recompresses them instead of copying them as they are, so they
are not rewritten twice.

**Costs:** each cold byte is written once more (SSD wear: about one extra
write of the data over its life). A random read of cold data decompresses
its 1 MiB unit (about 0.9 ms at 1.2 GB/s) instead of a 128 KiB chunk; the
safe-space cache then holds the unit, so neighbouring reads are free. Cold
units are not in the dedup index (their piece offsets are beyond its
64 KiB), so a later copy of cold data is not deduplicated against them.

`nanoer tier IMAGE` recompresses everything cold at once, offline: for
tests, and for preparing images (stage 2's compressed OS image).

## 4. Folder policies

An extended attribute on a directory or a file, `user.nanoer.policy`
(the `user.nanoer.` prefix is Nanoer's since M4; this one name becomes
settable):

| Policy | New data | Cold data |
|---|---|---|
| `none` | stored raw | stays raw |
| `fast` | LZ4 | stays LZ4 (reads fastest) |
| `balanced` (default) | zstd 1 | zstd 9, 1 MiB units, after 7 days |
| `max` | zstd 3 | zstd 19, 1 MiB units, after 1 day |

- **Inherited at creation:** a new file or directory takes its parent's
  policy (copied as its own attribute), so a whole tree follows the folder
  it was created in. Changing a folder's policy later changes what is
  written from then on; `nanoer policy --recursive PATH VALUE` applies it
  to everything already inside.
- `nanoer policy PATH` shows the policy in force; `nanoer stats` reports
  space by policy.
- Invalid values are refused (`EINVAL`).

## 5. Format changes (`docs/format.md`)

| Change | Why |
|---|---|
| extended reference flag 3: `cold` (the chunk has been recompressed as cold) | the pass never examines a chunk twice, with no header reads |
| a new `features_incompat` bit, `cold_data`, set by mkfs | an M4 reader would reject the unknown flag as damage; refusing to mount is clearer |
| the meaning of `user.nanoer.policy` (values above) | other implementations must apply the same policies |
| packs whose pieces are one file's consecutive chunks (cold units) | already allowed; documented |

M4 filesystems keep working, with policies for new writes but without
cold recompression (decision 6), as M3 ones did for dedup.

## 6. The cache: measure, then decide

The safe space is an LRU. A single large scan (a backup, a virus scan, an
indexer) evicts the chunks everything else was using. A **scan benchmark**
measures it: a working set read repeatedly while a scan of 4× the safe
space runs; hit rate and time with the LRU, and with a 2Q cache
(chunks seen once go to a small queue, only chunks seen again enter the
main one). 2Q replaces the LRU only if it improves the working set's hit
rate by at least a quarter without slowing the scan (decision 5). The
kernel's page cache sits above the safe space and has its own protection,
so the gain may well be small.

## 7. Tests

| Test | How | Where |
|---|---|---|
| **Tiering unit tests** | cold detection; units of eight chunks and shorter ones; a file written while being recompressed keeps its new data; a deleted one is dropped; shared (deduplicated) chunks and packed small files; the `cold` flag; GC recompressing cold data it moves; nothing when space is short | here + CI |
| **Policies** | each policy's codec for new data and cold form; inheritance by new files and folders; `--recursive`; invalid values; policy on an M4 filesystem | here + CI |
| **Crash points** (from M3) | a workload that recompresses (cold after 0 s): every crash point nightly, a sample per push | here + CI + nightly |
| **Model test** (from M2) | tiering passes mixed into the random operations, with remounts and power cuts | here + CI |
| **`fsck`** | crafted damage in cold units: found, reported by file and byte range, repaired | here + CI |
| **Image fuzzing** | the base image gets cold units | here + CI + nightly |
| **Real mount** | POSIX suite, fsx, `kill -9` with `cold_after=0` so recompression runs constantly | here + CI |
| **Scan benchmark** | §6 | here |

## 8. Files

```
include/nanoer/tier.h, src/core/tier.cpp     cold detection, the pass, cold units
include/nanoer/policy.h, src/core/policy.cpp policies: parsing, inheritance, codec choice
src/core/gc.cpp                              shared machinery; recompressing cold data it moves
src/core/fs.cpp                              codec per policy at commit, policy inheritance at create
src/core/metadata.cpp, include/nanoer/metadata.h   the `cold` flag
src/core/format.cpp, include/nanoer/format.h       feature bit
src/core/cache.cpp                           2Q, if the benchmark says so
src/core/check.cpp                           cold units
src/core/stats.cpp                           space by policy, cold share
src/tools/nanoer.cpp                         `nanoer tier`, `nanoer policy`
src/fuse/nanoerfs.cpp                        `-o cold_after=`
tests/unit/tier_test.cpp, policy_test.cpp, cache_test.cpp; crash, model, check tests extended
docs/format.md, KNOWLEDGE.md, README.md
```

## 9. Order of work

1. The `cold` flag and feature bit, `fsck` for cold units.
2. The tiering pass (online and `nanoer tier`), and GC recompressing.
3. Policies.
4. The scan benchmark, then 2Q only if it pays.
5. Measurements, documentation, report.

If a step shows the plan needs to change, I stop and bring it to you first.

## 10. Risks

| Risk | Mitigation |
|---|---|
| Recompression rewrites live data, so a bug loses data | the same copy-then-re-point machinery as GC, already crash-tested; the old data stays intact until two checkpoints later; crash exploration and the model test with tiering |
| It slows the machine while it runs | only when idle, throttled, paused on writes; zstd 9 at 43 MB/s per core |
| 1 MiB units make random reads of cold data slower | measured (§1.2); if it hurts, units of 256 KiB keep most of the gain (decided with numbers, brought to you first) |
| Extra writes wear SSDs | each cold byte rewritten once; `fast` and `none` folders are never recompressed |
| Space is short while the pass runs | it runs only with free space to spare, and stops before GC's low-water mark |
| The gain on media-heavy disks is small | expected: encoded media does not compress at any level; folders of it can be set to `none` |

## 11. Decisions needed

My recommendation is listed first in each.

1. **What counts as cold:** not written for 7 days, whatever the reads
   (recommended: zstd 9 reads as fast, and no access-time writes); or also
   not read for 30 days (needs access times written once a day per file,
   changing M2's decision).
2. **The cold form:** zstd 9 in 1 MiB units (recommended: 35.3 %, reads as
   fast, 1 MiB decompressed per random read); zstd 9 per 128 KiB chunk
   (37.6 %, nothing else changes); or zstd 19 in 1 MiB units for everything
   (33.2 %, but two days of CPU per 500 GB).
3. **Policies:** `none`, `fast`, `balanced`, `max` as in §4, stored as
   `user.nanoer.policy`, inherited at creation (recommended); or fewer
   (only `none` and the default).
4. **Read-ahead:** not in M5 (recommended: measured, decompression is not
   the bottleneck; the stage 3 kernel driver changes the picture); or
   build it anyway.
5. **Cache:** build the scan benchmark and switch to 2Q only if it pays
   (recommended); build 2Q regardless; or leave the LRU.
6. **Existing filesystems:** M4 ones keep working with policies but
   without cold recompression (recommended); or add a `nanoer upgrade`
   command.
