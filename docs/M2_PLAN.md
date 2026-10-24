# M2 plan: a working, mountable filesystem

**Status: APPROVED and COMPLETE (2026-10-24)**, with the recommended answers
to every question in §12. Part of stage 1 ([`STAGE1_PLAN.md`](STAGE1_PLAN.md)).
Results, measurements and the limits found are in `KNOWLEDGE.md` (§7.10,
§9.2, §14). Two changes made during the work, both recorded there: FUSE
`lseek` is not offered (it could make tools skip data), and the attribute
cache timeout is 60 s rather than 1 s.

## 1. Goal

`nanoerfs` mounts a Nanoer store on a folder, and programs use it like any
other filesystem. Everything written is stored compressed and is still there
after unmounting and mounting again.

```bash
mkfs.nanoer --size 20G home.img
nanoerfs home.img /mnt/data          # or: mount -t nanoer home.img /mnt/data
cp -a ~/projects /mnt/data/ && ls -l /mnt/data
fusermount3 -u /mnt/data
```

**Done when:**

1. All the operations in §6 work, checked by Nanoer's own POSIX test suite
   on a real FUSE mount (here and in CI), and by **pjdfstest** in CI.
2. Random-operation exercisers (Nanoer's own, plus **xfstests' fsx** in CI)
   find no data mismatch, including across unmount/remount.
3. Model-based tests (random operations checked against an in-memory
   reference, with remounts at random points) pass.
4. After `kill -9` of `nanoerfs`, everything that was `fsync`ed, or
   committed by the 5-second timer, is present after mounting again.
5. Real workloads work and are measured: copy a source tree, `tar` and
   `untar`, `git clone` and `git status`, compile a C project, play a video
   from the mount. Reported: space used, time compared with ext4.

## 2. Scope

**In M2:** the filesystem layer, persistence (log, transactions, checkpoints,
replay on mount), the read and write paths, the safe-space cache, the FUSE
adapter, mount/unmount tools, and reclaiming segments that become entirely
dead (§5.6, decision 1).

**Not in M2 (later milestones, as in the stage 1 plan):**

| Deferred | To |
|---|---|
| Power-cut simulation at every write point, `nanoer fsck` | M3 |
| Copying garbage collection, deduplication, small-file packing, `nanoer stats` | M4 |
| Hot/cold tiering, per-folder policies, read-ahead, 2Q/ARC cache | M5 |
| On-disk B+tree metadata (RAM no longer grows with the filesystem) | M6 |

M2 already has crash *recovery* (replaying the log on mount), since normal
mounting needs it. M3 is where it gets *proven* under every possible power
cut.

## 3. Components

```
 nanoerfs (src/fuse/)           FUSE low-level adapter: requests ↔ Filesystem calls, errno mapping
     │
 Filesystem (fs.h)              POSIX semantics: inodes, directories, links, rename, xattrs,
     │                          open/read/write/truncate/fsync, lookup counts, statfs
     ├── Metadata (metadata.h)  in-memory inodes, directory entries, extent maps, xattrs,
     │                          segment usage; applies and serialises operations
     ├── ChunkCache (cache.h)   the safe space: sharded LRU of decompressed chunks
     ├── WriteBuffer            dirty chunk data per file, flushed in commits
     └── Journal (journal.h)    the single append-only log: data chunks, metadata entries,
                                checkpoints, segment seals; segment allocation; replay
            └── M1: SegmentWriter / SegmentScanner / records / codecs / Store
```

`Filesystem` has a plain C++ API with no FUSE dependency, so nearly all
tests run in-process, quickly, under sanitizers, without mounting anything.

## 4. Metadata (in memory in M2)

| Structure | Contents |
|---|---|
| **Inode** | number, type and mode, uid, gid, nlink, size, atime/mtime/ctime/btime (ns), rdev, symlink target |
| **Directory** | sorted map name → inode, per directory inode |
| **Extent map** | per file: chunk index → location (record offset, stored length, uncompressed length). A missing chunk is a hole and reads as zeros. |
| **Xattrs** | per inode: name → value (values up to 64 KiB) |
| **Segment table** | per segment: state (free / log / sealed), sequence number, live data bytes |
| Counters | next inode number, next transaction id |

- Files are cut on a fixed grid of `chunk_size` (128 KiB by default): chunk
  *i* holds bytes `[i·chunk_size, (i+1)·chunk_size)`. Writing anywhere means
  rewriting only the affected chunk(s).
- **All-zero chunks become holes**: no record is written for them.
- Inode numbers are 64-bit and never reused in M2. The root is inode 1.
- **RAM estimate:** about 200 bytes per file plus about 64 bytes per chunk.
  A full 500 GB filesystem (~4 million chunks, ~1 million files) would need
  about 0.5 GB. That is acceptable for M2 testing and the reason for the
  on-disk B+tree in M6.

## 5. Persistence

Everything goes into **one append-only log**, spread over segments: data
chunk records, metadata entries, checkpoints, and segment seals.

### 5.1 New record types (added to `docs/format.md`)

| Type | Record | Contents |
|---:|---|---|
| 2 | **metadata log entry** | one transaction: a transaction id and a list of operations (create inode, set attributes, link, unlink, rename, set extent, truncate, set/remove xattr, free inode) |
| 3 | **checkpoint part** | a slice of a complete metadata snapshot. Large snapshots span several parts (and segments). The final part carries a checksum of the whole snapshot. |
| 4 | **segment seal** | "this segment is finished; the log continues in segment *N* with sequence number *S*" |

Operations are encoded compactly (type byte plus LEB128 variable-length
integers); the exact layout goes in `docs/format.md`.

### 5.2 Why the log is crash-safe: prefix consistency

When reading the log back, replay **stops at the first record that is not
intact**, and enters the next segment only through an intact seal. So after
a crash, the filesystem state is always exactly *some prefix* of the log, a
state that really existed. This holds even if the disk reordered writes,
because a missing earlier record cuts off everything after it. Data records
are always written before the metadata entry that refers to them, so
metadata can never point at missing data.

### 5.3 Commits and fsync

- Metadata operations take effect in memory immediately and queue up for the
  next **commit**.
- A commit: compress the dirty chunks (in parallel on a thread pool), append
  them in order, append one metadata log entry, hand everything to the store.
  When durability is needed, it ends with **one flush**.
- Commits happen on `fsync`/`fdatasync`, when dirty data passes a limit
  (default 64 MiB), and every **5 seconds** (decision 2), like ext4's
  default. Without `fsync`, a crash can lose at most the last ~5 seconds,
  never earlier data, and never consistency.
- `fsync` on any file commits everything queued before it. That is stronger
  than POSIX requires, simple, and correct.

### 5.4 Checkpoints

- A checkpoint writes the whole metadata snapshot into the log, then points
  the superblock at it.
- Taken when the log since the last checkpoint passes 256 MiB, on unmount,
  and at least every 5 minutes while busy.
- **mkfs now writes an initial checkpoint** (an empty root directory), so
  every filesystem always has one and mounting always has a starting point.
  This is a small change to M1's mkfs (decision 7).

### 5.5 Mount

1. Load the superblock (M1).
2. Read the checkpoint it points to, part by part, and verify the snapshot
   checksum.
3. Replay the log entries after it, following seals from segment to segment,
   until the log ends.
4. Clean up orphans: inodes with no links left (files deleted while open
   when the crash happened) are freed.
5. Continue the log in a fresh segment (never append to a segment that may
   end in a torn write).

### 5.6 Reclaiming space (decision 1)

Every rewrite or delete leaves dead chunks behind. M2 frees a segment when
**all** of its data is dead and its metadata entries are superseded by a
checkpoint. Segments that are only *partly* dead need copying GC (M4). So in
M2, workloads that rewrite files scattered across many segments can fill the
store with partly-dead segments and get `ENOSPC` early. That is acceptable
for M2 and fixed in M4.

**Safety rule:** a segment freed after checkpoint *C* becomes reusable only
once checkpoint *C+1* is durable. If the superblock write for *C* were torn,
mounting falls back to the previous superblock, whose checkpoint must still
exist.

## 6. POSIX behaviour

**FUSE operations implemented:** `init`, `destroy`, `lookup`, `forget`,
`getattr`, `setattr` (chmod, chown, utimens, truncate), `readlink`,
`mknod` (regular files, FIFOs, sockets, device nodes), `mkdir`, `unlink`,
`rmdir`, `symlink`, `rename` (including `RENAME_NOREPLACE` and
`RENAME_EXCHANGE`), `link`, `open`, `create`, `read`, `write`, `flush`,
`release`, `fsync`, `opendir`, `readdir`/`readdirplus`, `releasedir`,
`fsyncdir`, `statfs`, `setxattr`, `getxattr`, `listxattr`, `removexattr`,
`access`, `fallocate` (allocate and punch holes), `lseek` (`SEEK_DATA`,
`SEEK_HOLE`).

**Behaviour:**

- **Permissions** are enforced by the kernel (`default_permissions` mount
  option) from mode, uid and gid. This is standard for FUSE filesystems and
  avoids reimplementing access checks.
- **Deleted-but-open files** keep working until the last close. FUSE lookup
  counts are tracked, and an inode is freed when it has no links and no
  lookups.
- **Hard links, symlinks, special files, sparse files** are all supported.
- **`mmap`**, including shared writable mappings, works through the kernel
  page cache (writeback cache mode).
- **Timestamps** have nanosecond precision. **atime** is not updated on
  reads (`noatime` behaviour, decision 3), which avoids a metadata write for
  every read. mtime and ctime follow POSIX.
- **`readdir`** serves a snapshot taken at `opendir`, so entries added or
  removed meanwhile cannot make a listing skip or repeat names.
- **`statfs`** (`df`) reports *physical* space: total segment space and free
  segments.
- **Xattrs** work in all namespaces (`user.`, `trusted.`, `security.`), so
  SELinux labels and file capabilities work. **POSIX ACLs** are stored but
  not enforced in M2 (decision 4).
- **Not supported in M2:** file locking beyond the kernel's local locks,
  `O_DIRECT`, NFS export, quotas.

## 7. FUSE adapter and tools

- **libfuse 3 low-level API**, multi-threaded session loop.
- **Writeback cache** on (the kernel batches small writes), requests up to
  1 MiB, attribute and entry cache timeouts of 1 second.
- **`nanoerfs IMAGE MOUNTPOINT [-o options]`**, plus a `mount.nanoer`
  helper so `mount -t nanoer` and `/etc/fstab` work.
- **Options:**

  | Option | Meaning |
  |---|---|
  | `safe_space=SIZE` | decompressed-chunk cache budget (default 256M) |
  | `commit=SECONDS` | commit interval (default 5) |
  | `ro` | read-only |
  | `allow_other` | let other users access the mount |
  | `-f` | stay in the foreground (debugging) |

- **Unmount** (`fusermount3 -u` or `umount`): final commit, then a
  checkpoint and superblock update.
- `nanoer info` additionally reports the checkpoint, the number of files and
  the log length since the last checkpoint.

**Concurrency:** one reader-writer lock over metadata in M2 (many readers,
one writer), per-file locks for data, and compression in parallel on a
thread pool. Finer-grained locking comes later, if benchmarks call for it.

## 8. Tests

| Kind | What | Where it runs |
|---|---|---|
| Unit | metadata operations and their encoding; checkpoint encoding; journal (seals, allocation, replay); cache eviction and budget; write buffer | here + CI |
| **Model-based** | thousands of random operations (create, write, truncate, rename, link, unlink, xattr, …) on `Filesystem` and on a simple in-memory reference model, compared after every step; remount at random points | here + CI |
| Persistence | operations → unmount → mount → identical state; `kill -9` style (drop the process state, keep only flushed writes) → state of the last commit | here + CI |
| **POSIX suite** | Nanoer's own GoogleTest suite run against a real mount: every operation in §6, error codes (`EEXIST`, `ENOENT`, `ENOTEMPTY`, `EXDEV`, …), permissions as a non-root user | here + CI |
| **Exerciser** | `nanoer-fsx`: random reads, writes, truncates, hole punches and mmap writes on files in the mount, checked against a shadow copy | here + CI |
| pjdfstest | the standard POSIX conformance suite (built from its GitHub source) | **CI only** (not packaged for Ubuntu; GitHub is blocked here) |
| xfstests fsx | the classic data-integrity exerciser | **CI only** (same reason) |
| fio | `verify` mode: write, read back, check | here + CI |
| Workloads | §1 item 5, with timings and space against ext4 | here, reported |

All of it also runs under ASan/UBSan and TSan.

## 9. Files

```
include/nanoer/  metadata.h  journal.h  cache.h  fs.h  thread_pool.h
src/core/        metadata.cpp  journal.cpp  checkpoint.cpp  cache.cpp  fs.cpp  thread_pool.cpp
                 mkfs.cpp (initial checkpoint)
src/fuse/        nanoerfs.cpp (adapter + main), mount_nanoer.cpp (mount helper)
src/tools/       nanoer.cpp (info additions), nanoer_fsx.cpp
tests/unit/      metadata_test, journal_test, cache_test, fs_test, fs_model_test, persistence_test
tests/integration/  posix_test.cpp, run_pjdfstest.sh, run_fsx.sh, workloads.sh
docs/format.md   record types 2, 3, 4; operation encoding; initial checkpoint
KNOWLEDGE.md     every M2 decision, the FUSE details used, measurements
.github/workflows/ci.yml   integration job: FUSE mount, pjdfstest, fsx
```

## 10. Order of work

1. Metadata structures, operation encoding, `Filesystem` API, model-based
   tests (in memory, no persistence).
2. Journal: log writer, seals, checkpoints, mount/replay, segment reclaim,
   mkfs initial checkpoint; persistence tests.
3. Data path: write buffer, commits, thread pool, safe-space cache.
4. FUSE adapter and mount tools; POSIX suite and exerciser on a real mount.
5. CI integration (pjdfstest, fsx), workloads, measurements, documentation,
   report.

If any step shows the plan needs to change, I stop and bring the change to
you first.

## 11. Risks

| Risk | Mitigation |
|---|---|
| POSIX corner cases (rename, hard links, deleted-but-open files) | model-based tests, the POSIX suite, pjdfstest |
| Writeback cache subtleties (size and mtime owned by the kernel) | follow libfuse's documented rules; fsx and mmap tests |
| Space exhaustion from partly-dead segments before M4 GC | documented limit; test stores are large sparse images |
| RAM grows with filesystem size | documented limit; M6 |
| Slow under FUSE | measured and reported; tuning in M5, kernel driver in stage 3 |

## 12. Decisions needed

My recommendation is listed first in each.

1. **Space reclaim in M2:** free *entirely dead* segments now (recommended;
   cheap and safe, and keeps long tests from filling up), or nothing until
   M4.
2. **Commit interval:** 5 seconds by default, configurable (recommended,
   like ext4), or commit on every write (much slower).
3. **atime:** not updated on reads (recommended), or `relatime` (updated at
   most once a day, one extra metadata write per file per day).
4. **POSIX ACLs:** stored but not enforced in M2, enforced in a later
   milestone (recommended), or enforced in M2.
5. **Locking:** one metadata lock in M2, finer later only if benchmarks need
   it (recommended).
6. **pjdfstest and xfstests' fsx in CI only**, with Nanoer's own POSIX suite
   and exerciser running here too (recommended; the only option, since
   neither suite can be installed in this environment).
7. **mkfs writes an initial checkpoint** (recommended): a small change to
   M1's mkfs and the draft format, so every filesystem always has a
   checkpoint.
8. **Names:** `nanoerfs` for the mount program, `mount -t nanoer` for
   fstab (recommended).
