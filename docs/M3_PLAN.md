# M3 plan: crash safety, proven, and `nanoer fsck`

**Status: PROPOSED, waiting for approval.** Part of stage 1
([`STAGE1_PLAN.md`](STAGE1_PLAN.md)). No M3 code is written until this plan
is approved.

## 1. Goal

Make "it survives power cuts and damage" a tested fact, not a design claim,
and give users a tool to check and repair a filesystem.

**Done when:**

1. **Every crash point is safe.** For a set of workloads, a power cut after
   *every single* write to the store, in many variations (unflushed writes
   lost, kept, reordered, torn), always gives a filesystem that mounts, that
   `fsck` finds clean, whose contents are exactly the state of one real
   commit, and that contains everything that was `fsync`ed.
2. **Disk errors are safe.** A failing write or flush at any point makes
   Nanoer stop accepting changes (never write inconsistent state); after a
   restart, the filesystem is consistent.
3. **Damage is detected, never silently lost.** Bit rot or overwritten
   blocks anywhere in the store either cause a clean error (`EUCLEAN` for
   damaged file data) or are reported by mount and `fsck`, never a crash,
   never wrong data returned, and never a quietly shortened log.
4. **`nanoer fsck`** checks every invariant of the format, repairs what can
   be repaired safely, and reports exactly what was lost.
5. **Corrupted-image fuzzing:** mount and `fsck` survive millions of
   randomly damaged images without crashing or hanging.
6. **Real processes:** `kill -9` of `nanoerfs` in the middle of real
   workloads, on a real mount, always recovers with everything `fsync`ed.

**Not in M3:** copying garbage collection (M4), redundancy (Nanoer keeps one
copy of everything, so damage can be *detected* but damaged data cannot be
*restored*; see §9), online scrubbing of a mounted filesystem, Windows.

## 2. What exists, and the gaps

M2 already has: prefix-consistent replay (a crash recovers a state that
existed), checksums on every record, journal tests with random lost and torn
writes, filesystem crash tests that drop everything unflushed, and a
read-only fallback when a commit fails.

| Gap | Why it matters |
|---|---|
| Crash tests sample a few random crash points | a bug at one specific write (say, between a seal and the next header) could be missed |
| **Damage in the middle of the log looks like the end of the log** | replay stops there, so every later transaction would be silently dropped, and the next mount would write over them |
| A damaged latest checkpoint makes the filesystem unmountable | the previous checkpoint is still on disk and could be used |
| Both superblocks lost means everything is lost | the checkpoints and data are still in the segments |
| Write and flush errors are tested only for `FaultStore` itself | not "error at every point of a commit, checkpoint, mount" |
| No `fsck` | no way to check a filesystem, or repair it, offline |

## 3. Engine changes

1. **Telling damage from a torn tail.** When replay stops at a record that is
   not intact, it looks further ahead in the same segment, at the following
   8-byte-aligned positions, for intact records with the segment's sequence
   number, and checks whether the log continues into later segments. A torn
   write can only be at the very end of what was written, so intact records
   after the stopping point mean **damage**. Mount then refuses to write: it
   mounts **read-only** and asks for `fsck` (decision 1).
2. **Checkpoint fallback.** If the newest checkpoint is damaged, mount uses
   the checkpoint the other superblock slot points to and replays forward
   from there (skipping the damaged checkpoint's parts). The two-checkpoint
   reuse rule (`docs/format.md` §10.4) guarantees the older checkpoint and
   the log after it still exist.
3. **"Errors" flag in the superblock.** Set when Nanoer meets damage at run
   time (a chunk failing its checksum, damage found at mount), cleared by a
   successful `fsck`. `nanoer info` and mount warn while it is set. This
   uses a reserved superblock field, so older versions ignore it (decision 4).
4. **Damaged file data is reported precisely.** A read of a damaged chunk
   returns `EUCLEAN` (as now) and is logged with the file's inode number and
   byte range; `nanoer info` counts such errors.
5. **Write ordering review.** Every place where order matters is listed in
   `KNOWLEDGE.md` and covered by a crash test: data before the transaction
   referring to it; checkpoint durable before the superblock pointing to it;
   superblock durable before segments are reused; seal before the next
   segment is used; one flush per commit.

## 4. `nanoer fsck`

```
nanoer fsck IMAGE              check only (default); changes nothing
nanoer fsck --repair IMAGE     check and repair what is safe to repair
nanoer fsck --deep IMAGE       also read and verify every data chunk
```

Also installed as **`fsck.nanoer`**, so `fsck -t nanoer` and the boot-time
check from `/etc/fstab` work (decision 6). Refuses to run on a mounted
filesystem (the store is locked). Exit codes follow e2fsck: 0 clean,
1 errors fixed, 4 errors left, 8 operational error.

**Checks, in phases:**

| Phase | Checks |
|---|---|
| 1. Superblocks | both slots; generations; geometry matches the store |
| 2. Segments and log | headers (UUID, index, unique sequence numbers); the log from the checkpoint: seal chain, consecutive transaction ids, damage versus torn tail (§3.1) |
| 3. Directory tree | everything reachable from the root; no cycles; `..` correct; no hard links to directories; names valid; link counts of files and directories correct; orphans |
| 4. Space | every chunk points at an intact data chunk record in a segment in use, of the right size, whose recorded owner (inode, offset) matches; no chunk referenced twice; no chunks past the end of a file; live bytes per segment add up |
| 5. Data (`--deep`) | every chunk decompresses and matches its checksum |

**Repairs** (`--repair`) work like a mount: load the metadata, fix it in
memory, then write a new checkpoint and point the superblock at it. Nothing
is overwritten in place.

| Problem | Repair |
|---|---|
| wrong link count | set to the real count |
| unreachable file or directory | reconnect it in `/lost+found` as `#<inode>` |
| entry pointing at a missing inode | remove the entry |
| chunk that is damaged, missing, or past the end of the file | remove it (that range reads as zeros) and **list the file and byte range** |
| wrong space accounting | recompute |
| orphans | free them |
| damage in the middle of the log | keep the intact prefix (the state up to the damage), and report how many later transactions are lost (decision 2) |
| damaged newest checkpoint | rebuild from the older checkpoint plus the log (§3.2) |
| **both superblocks lost** | find the newest complete checkpoint by scanning the segments and write new superblocks (decision 3) |

`fsck` never "repairs" by guessing at data: what cannot be read back intact
is reported as lost, by file and byte range.

## 5. Tests

| Test | How | Where |
|---|---|---|
| **Crash-point exploration** | `FaultStore` records every write and flush. A workload (creates, writes, renames, deletes, fsyncs, many small segments so seals, checkpoints and segment reuse happen often) is replayed, and for **every write** a crash image is built in several variants: all unflushed writes lost; all kept; each one dropped alone (reordering); the last one torn at every sector boundary; random subsets. Each image must mount, be clean under `fsck`, equal the state of one real commit, and contain everything `fsync`ed. In the style of CrashMonkey and ALICE (research tools for exactly this). | here + CI |
| Mount-time crashes | the same, for crashes during mount itself (sealing the old log, orphan cleanup) and during `fsck --repair` | here + CI |
| **I/O errors** | a write or flush fails at every position in the same workloads; Nanoer must refuse further changes, and the durable image must mount consistent | here + CI |
| `fsck` | for every inconsistency in §4, a test that creates it, checks `fsck` finds it, repairs it, and finds the result clean | here + CI |
| **Damage** | flip bits and zero blocks in data records, transactions, checkpoints, seals, headers, superblocks: never a crash or wrong data; the right error; `fsck` reports it | here + CI |
| **Image fuzzing** | a libFuzzer target that damages a valid image at fuzzer-chosen places, then mounts it, reads everything, and runs `fsck --repair` and `fsck`: no crash, no hang, and after repair `fsck` is clean | here + CI smoke run |
| **`kill -9` on a real mount** | a writer that `fsync`s and logs what it synced; `nanoerfs` is killed at random moments during it, fsx, untar and builds; remount; everything logged as synced is there, and `fsck` is clean | here + CI |

**Time budget (decision 5):** CI runs a fixed sample in a few minutes per
push. A **nightly CI workflow** runs the exhaustive crash exploration and
longer fuzzing (this also catches rare failures that per-push runs miss).

## 6. Files

```
include/nanoer/check.h, src/core/check.cpp   fsck: phases, report, repairs
src/core/journal.cpp                          damage detection, checkpoint fallback
src/core/format.cpp, include/nanoer/format.h  superblock errors flag
src/core/fs.cpp                               read-only on damage, error flag, error reporting
src/tools/nanoer.cpp                          `nanoer fsck`; fsck.nanoer link
tests/unit/crash_test.cpp, io_error_test.cpp, check_test.cpp, damage_test.cpp
fuzz/fuzz_image.cpp
tests/integration/kill_test.sh
.github/workflows/nightly.yml                 exhaustive crash runs, long fuzzing
docs/format.md                                errors flag; recovery rules
KNOWLEDGE.md                                  write-ordering list, decisions, results
```

## 7. Order of work

1. Crash-point exploration and I/O-error harness first, run against the M2
   engine: find real bugs before building on it.
2. Replay hardening: damage detection, checkpoint fallback, errors flag.
3. `fsck` checks and the CLI.
4. `fsck` repairs, `/lost+found`, superblock rescue.
5. Damage tests, image fuzzing, `kill -9` tests, CI and nightly workflow,
   documentation, report.

If a step shows the plan needs to change, I stop and bring it to you first.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Exhaustive crash runs are slow | small stores and segments; a fixed sample per push, everything nightly |
| Damage detection gives false alarms on a genuinely torn tail | a torn write can only be at the end; tests cover torn tails at every sector |
| Repair makes things worse | repairs only write a new checkpoint (old data stays on disk until the next checkpoint cycle); `fsck` runs again after repair in every test |
| Simulated power cuts differ from real disks | `FaultStore` models the standard guarantee (data is durable only after a flush; unflushed writes may be lost, reordered or torn); real-device testing (e.g. `dm-log-writes`) is left for stage 2 (decision 7) |

## 9. Not covered: redundancy

Nanoer stores one copy of everything. M3 makes damage *detected and
reported*, but a damaged chunk's data is gone, as on ext4 or XFS. Btrfs and
ZFS can keep two copies of metadata (and optionally data) to repair
themselves. Keeping two copies of the checkpoint would be cheap and could
come later. It is not part of M3.

## 10. Decisions needed

My recommendation is listed first in each.

1. **Damage in the middle of the log at mount:** mount read-only and ask for
   `fsck` (recommended), or mount read-write, accepting the loss.
2. **Repairing a damaged log:** keep the intact prefix and report what was
   lost (recommended); trying to salvage transactions after the damage is
   risky (they may depend on the damaged one) and is not in M3.
3. **Superblock rescue** (both superblocks lost, found by scanning for
   checkpoints): include in M3 (recommended), or postpone.
4. **"Errors" flag in the superblock**, set on damage, cleared by `fsck`:
   yes (recommended).
5. **Test budget:** a sample per push plus a nightly exhaustive run
   (recommended), or everything per push (slow), or local only.
6. **`fsck.nanoer`** for `fsck -t nanoer` and boot-time checks: yes
   (recommended).
7. **Real-device power-cut testing** (`dm-log-writes`): postpone to stage 2
   (recommended), or try it in CI now.
