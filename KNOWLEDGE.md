# Nanoer Knowledge Base

The technical reference for Nanoer: what it is built on, how each piece works,
which algorithms it uses, and **why** each choice was made. Keep it current:
every new technology, algorithm, design decision, measurement or rejected
alternative gets recorded here (see [§15 Decision log](#15-decision-log)).

Facts marked *(approx.)* are typical published figures that vary with CPU and
data; Nanoer's own measurements are in [§14](#14-measurements).

## Contents

1. [What Nanoer is](#1-what-nanoer-is)
2. [Compression fundamentals](#2-compression-fundamentals)
3. [Codecs](#3-codecs)
4. [Chunking and random access](#4-chunking-and-random-access)
5. [Integrity: checksums and hashes](#5-integrity-checksums-and-hashes)
6. [Deduplication](#6-deduplication)
7. [Storage engine design](#7-storage-engine-design)
8. [Caching: the safe space and read-ahead](#8-caching-the-safe-space-and-read-ahead)
9. [Linux internals Nanoer depends on](#9-linux-internals-nanoer-depends-on)
10. [Existing systems and how Nanoer differs](#10-existing-systems-and-how-nanoer-differs)
11. [Tech stack and reasons](#11-tech-stack-and-reasons)
12. [Testing and verification techniques](#12-testing-and-verification-techniques)
13. [Licensing](#13-licensing)
14. [Measurements](#14-measurements)
15. [Decision log](#15-decision-log)
16. [History: the Python prototype (format v1)](#16-history-the-python-prototype-format-v1)
17. [Glossary](#17-glossary)
18. [References](#18-references)

---

## 1. What Nanoer is

**Goal:** transparent, OS-level compression. With identical contents, a system
that needs **800 GB** without Nanoer should need about **500 GB** with it.
Every program (the OS, apps, games, tools) keeps reading and writing normal
files and never knows the data is compressed.

**Core idea, live extraction:** data is stored as independently compressed
chunks. When a program reads a byte, only the chunk containing it is
decompressed, into a bounded memory budget called the **safe space**. Nothing
is ever fully extracted to disk.

**Roadmap:**

| Stage | What | Where it runs |
|---|---|---|
| 1 | C++ engine + read-write FUSE filesystem (`nanoerfs`) for folders such as `/home`, `/opt`, games | user space |
| 2 | Whole OS: compressed read-only system image + writable Nanoer layer, booted from initramfs | user space + boot |
| 3 | Kernel driver (C or Rust) using the same on-disk format, for full speed | kernel |

Linux first. Windows and macOS come later.

**Status:** stage 1, milestones **M1 (foundation)** and **M2 (working
filesystem)** complete (2026-10-24). `nanoerfs` mounts a Nanoer store
through FUSE as a normal read-write folder; data and metadata survive
unmounting and power cuts. Next: M3, crash safety proven under every
possible power cut, and `nanoer fsck`.

**Saving 37.5 % of the disk (800 → 500) needs an average compression ratio of
1.6 : 1.** That is realistic for OS files, applications, code and documents
(measured at 42.5 % of original size with fast zstd, 37.2 % with maximum
zstd, see §14) and impossible for already-encoded media
(about 1 : 1). Total savings depend on what is on the disk.

---

## 2. Compression fundamentals

### 2.1 Why data compresses at all

Lossless compression removes **redundancy**. Claude Shannon (1948) showed the
minimum average size of data is set by its **entropy**: the less predictable
the data, the less it can shrink. Two kinds of redundancy matter in practice:

1. **Repetition:** the same byte sequences recur (code, text, binaries,
   uncompressed images). Removed by **dictionary/LZ** methods.
2. **Skewed symbol statistics:** some bytes or tokens are far more common
   than others. Removed by **entropy coding**.

Modern codecs combine both: an LZ stage finds repeats, then an entropy coder
packs what remains.

### 2.2 LZ77 (Lempel & Ziv, 1977)

The encoder slides a **window** over previously seen data. At each position it
looks for the longest earlier match and emits either a **literal** (a raw
byte) or a **(distance, length)** pair: "copy `length` bytes from `distance`
bytes back". A bigger window finds more distant repeats (better ratio) but
needs more memory and search time. Match finders use hash chains or binary
trees. Decompression is just copying, which is why LZ decoders are fast.

### 2.3 Entropy coding

- **Huffman coding (1952):** gives frequent symbols shorter bit codes. Fast,
  but every code is a whole number of bits, which wastes space when one
  symbol is very likely.
- **Arithmetic / range coding:** encodes the whole message as one number in
  an interval, reaching fractions of a bit per symbol. Excellent ratio, but
  slower. Used by LZMA.
- **ANS, Asymmetric Numeral Systems (Jarek Duda, ~2009–2014):** arithmetic-
  coding efficiency at Huffman-like speed. **tANS** (table-based) is
  implemented as **FSE** (Finite State Entropy) by Yann Collet and used in
  zstd. **rANS** is the range variant.
- **Context modeling:** predicts each bit or byte from the bytes before it
  (LZMA does this with Markov-chain contexts). It gives better predictions and
  smaller output, but costs CPU time.

### 2.4 Why encoded media does not compress

H.264/H.265/AV1 video, AAC/MP3/Opus audio and JPEG/PNG/WebP images are
already compressed by codecs specialised for that media. Their output is close
to random (maximum entropy). General-purpose compressors find almost nothing:
we measured an H.264 MP4 at **99.8 %** of its original size (§14). Nanoer
detects such chunks and **stores them raw**, so they cost nothing extra but
save nothing either. Shrinking media requires *re-encoding* with a better
media codec, which is lossy and out of Nanoer's scope.

### 2.5 The three-way trade-off

Every codec trades **ratio** against **compression speed** against
**decompression speed**. For a filesystem, **decompression speed matters most**,
because data is read far more often than it is written. That is why zstd and
LZ4 dominate modern filesystems.

---

## 3. Codecs

Typical single-core figures *(approx.)*. Exact numbers depend heavily on data
and CPU. **Measured on Linux system files with Nanoer's own benchmark: see
§14.**

| Codec | Method | Compress | Decompress | Ratio | Nanoer role |
|---|---|---|---|---|---|
| **LZ4** | LZ77 only, no entropy stage | ~500–800 MB/s | ~3–5 GB/s | lowest | latency-critical data (games, hot files) |
| **zstd** levels 1–3 | LZ77 + Huffman + FSE | ~300–500 MB/s | ~1–1.5 GB/s | good | **default** for written data |
| **zstd** levels 15–19 | same, deeper search | ~5–30 MB/s | ~1–1.5 GB/s | very good | background recompression of cold data |
| **LZMA / xz** | LZ77, large window + range coder + context modeling | ~1–5 MB/s | ~50–150 MB/s | best | archival, rarely read data |
| DEFLATE / zlib | LZ77 (32 KiB window) + Huffman | ~20–60 MB/s | ~300–400 MB/s | moderate | not used (zstd beats it everywhere) |
| none | stored raw | — | memcpy | 1 : 1 | incompressible chunks |

### 3.1 LZ4 (Yann Collet, 2011)

A pure LZ77 byte-oriented format with no entropy coding, so the decoder is
little more than a copy loop. **Why Nanoer uses it:** when a game streams
assets, the slowest step should be the disk, not the CPU. LZ4 adds almost no
latency. Library licence: BSD-2-Clause.

### 3.2 Zstandard / zstd (Yann Collet, Facebook, 1.0 in 2016)

An LZ77 stage feeding Huffman (for literals) and FSE/tANS (for match lengths,
offsets and literal lengths). Key properties:

- **Levels −7 to 22:** higher levels search harder. **Decompression speed
  stays roughly the same at every level**, so data can be written fast now
  and recompressed harder later without slowing reads. This is the basis of
  Nanoer's hot/cold tiering (§7.7).
- **Dictionaries:** a trained dictionary greatly improves the ratio on small,
  similar inputs (config files, small game assets). Planned for small-file
  packing.
- **Long-distance matching (`--long`):** finds repeats far apart in large
  inputs.
- Used by Btrfs, ZFS, squashfs, EROFS, the Linux kernel itself, and package
  formats (Arch, Fedora RPM).

Library licence: BSD **or** GPL-2.0 (dual).

### 3.3 LZMA / xz (Igor Pavlov, 7-Zip)

LZ77 with a very large window (up to 1.5 GiB), plus a range coder driven by
bit-level context models. It has the best ratio of the three, but
decompression is slow (~10× slower than zstd). **Nanoer role:** only for data
that is cold enough that ratio beats read speed. Library: liblzma (XZ Utils),
licence 0BSD in current releases.

> **Security lesson:** XZ Utils 5.6.0 and 5.6.1 shipped a deliberately planted
> backdoor (CVE-2024-3094, found in March 2024). Nanoer pins dependency
> versions, builds them from known sources, and documents each one here.

### 3.4 How Nanoer calls the codecs (M1 implementation)

- **LZ4:** the raw **block** format (`LZ4_compress_default`, or
  `LZ4_compress_HC` for levels 3–12), not the LZ4 frame format. Nanoer's
  record header already stores sizes and checksums, so a frame would only add
  overhead.
- **zstd:** one frame per chunk. Compression and decompression contexts are
  created once **per thread** and reused (`thread_local`), because creating
  a context costs more than compressing a small chunk.
- **LZMA:** an **.xz stream with check type None** (Nanoer has its own
  checksums) holding a single LZMA2 filter. The xz container was chosen
  because the Linux kernel's xz decoder reads it directly (stage 3). The
  LZMA2 **dictionary is shrunk to the chunk size**: liblzma's one-shot API
  otherwise allocates the preset's full dictionary (8 MiB at preset 6) for
  every call, which is pure waste for a 128 KiB chunk. The decoder refuses
  streams needing more than 128 MiB of memory, so damaged data cannot cause
  huge allocations.
- **Null pointers:** an empty buffer may have a null data pointer, and
  liblz4's HC compressor crashes on null even with length 0 (found by
  Nanoer's tests). Nanoer passes a valid dummy address for empty buffers to
  every codec.
- **Kernel availability:** the kernel already contains decoders for LZ4,
  zstd and xz (`lib/lz4`, `lib/zstd`, `lib/xz`). No other codec is used,
  so a stage 3 kernel driver needs no new decompressors.

### 3.5 Incompressibility detection

Compressing data that will not shrink wastes CPU. Strategies, cheapest first:

1. Policy: known-incompressible file types (`.mp4`, `.jpg`, `.zip`, …).
2. A **byte-entropy estimate** on a sample of the chunk (a byte histogram
   gives Shannon entropy; close to 8 bits/byte means random).
3. A fast trial with LZ4 or zstd level 1 on a sample.
4. Always: if the output is not smaller than the input, **store raw**. The
   format records a per-chunk "stored" flag.

Btrfs uses a similar heuristic.

---

## 4. Chunking and random access

A single compressed stream must be decoded from the beginning. Splitting data
into **independently compressed chunks** makes any byte reachable by
decompressing only one chunk. This is the foundation of live extraction.

**Chunk size is the central trade-off:**

| Smaller chunks (e.g. 64–128 KiB) | Larger chunks (e.g. 1–16 MiB) |
|---|---|
| fast random reads (little to decompress per read) | slightly better ratio (more context per chunk) |
| cheap rewrites (small read-modify-write) | expensive rewrites: changing 1 byte means recompressing the whole chunk |
| more index entries (more metadata) | less metadata |

**Read amplification:** reading 4 KiB from a 1 MiB chunk decompresses 1 MiB.
**Write amplification:** changing 4 KiB inside a chunk rewrites the whole
compressed chunk.

**Measured (§14):** on Linux system files, 4 MiB chunks compressed only about
1.5 percentage points better than 128 KiB chunks. So, for a writable
filesystem, **moderate chunks (~128 KiB) are the right default**. Larger
chunks are worthwhile only for cold or read-only data (the stage 2 system
image).

**Fixed-size output** (used by EROFS) inverts the usual approach: instead of
compressing fixed-size input, it compresses as much input as fits in a fixed
4 KiB output block, aligning compressed data to disk blocks and reducing read
amplification. Nanoer keeps this as an option for the read-only image.

---

## 5. Integrity: checksums and hashes

Compression amplifies corruption: one flipped bit can destroy a whole chunk.
So every stored chunk is checksummed, and Nanoer **never returns unverified
data**.

| Algorithm | Type | Speed *(approx.)* | Use in Nanoer |
|---|---|---|---|
| **CRC-32C** (Castagnoli) | error-detecting code | many GB/s with the SSE4.2 / ARMv8 CRC instructions | metadata blocks, superblock |
| **XXH3** (xxHash, stable since 0.8.0, 2020) | non-cryptographic hash | tens of GB/s | per-chunk data checksum |
| **BLAKE3** (2020) | cryptographic hash | GB/s, SIMD and multithreaded | deduplication identity |

- **CRC** is designed to catch random bit errors and is hardware-accelerated.
  Btrfs and ext4 (`metadata_csum`) use CRC-32C.
- **XXH3** is far faster than any CRC on large inputs and has good
  distribution, but it is **not** collision-resistant against an attacker.
  That is fine for detecting accidental corruption.
- **BLAKE3** is collision-resistant. Deduplication treats "same hash" as "same
  data", so a weak hash would let a crafted or unlucky collision **silently
  replace one file's contents with another's**. Hence a cryptographic hash.
  Alternative: a weak hash plus byte-for-byte comparison on match, which
  requires reading and decompressing the candidate and is too slow.
- The checksum covers the **uncompressed** data, and verification happens
  after decompression. This catches codec bugs as well as disk errors.
- Every record also carries a **CRC-32C of its stored payload** and one of
  its header. These let scans (log replay, `fsck`, GC) check a record without
  decompressing it, and detect torn writes at the end of the log.

---

## 6. Deduplication

Identical data stored once, referenced many times. Common in OS images
(duplicate libraries, locale files), games (repeated assets), and backups or
VM images.

- **Fixed-size dedup:** hash each fixed chunk and look it up in a hash → chunk
  index. Simple, but inserting one byte shifts every later chunk boundary, so
  nothing matches any more.
- **Content-defined chunking (CDC):** boundaries are chosen by the *content*
  using a rolling hash. A cut happens where the hash of the last few bytes
  matches a pattern, so an insertion only changes nearby chunks.
  - **Rabin fingerprints:** classic rolling hash (used by LBFS, 2001).
  - **Gear hash / FastCDC** (Xia et al., USENIX ATC 2016): much faster
    rolling hash plus "normalized chunking", which keeps chunk sizes near a
    target. This is the modern standard (restic, borg-like tools).
- **Costs:** the dedup index uses RAM or disk (about 32–64 bytes per unique
  chunk). Chunks need **reference counts** so a shared chunk is freed only
  when its last user goes away. Dedup interacts with garbage collection
  (§7.6).

**Nanoer plan:** BLAKE3-identified chunks with reference counts. Fixed-size
chunking for the writable filesystem (writes arrive at arbitrary offsets,
which makes CDC awkward). FastCDC when building read-only images and during
cold-data recompaction.

---

## 7. Storage engine design

### 7.0 Nanoer's on-disk format (version 1)

The byte-level specification is `docs/format.md`. Its main design choices:

- **Two superblocks, alternating.** Generation `g` goes to slot `g mod 2`, so
  writing a new superblock never touches the newest valid one. A torn
  superblock write therefore cannot lose the filesystem (tested by simulating
  power cuts mid-write).
- **Never fall back past an unsupported superblock.** If the newest intact
  superblock is from a newer format version, mounting is refused. Using the
  older slot instead would silently throw away newer changes.
- **Feature flags** in three classes, as in ext4: compat (ignore),
  ro_compat (mount read-only), incompat (refuse).
- **Segments carry the filesystem UUID and a sequence number.** A segment
  is in use only if its header names this filesystem. So `mkfs` does not
  need to clear segments, and stays instant even on a 1 TB disk.
- **Records carry their segment's sequence number.** When GC frees a segment
  and the log reuses it, old records left behind beyond the new log's end
  still look intact, but have an older sequence number. The scanner rejects
  them. Without this, stale data could be replayed after a crash.
- **8-byte record alignment.** 4 KiB alignment would waste on average 2 KiB
  per chunk (over 5 % on compressed 128 KiB chunks); 8 bytes keeps integer
  fields aligned at negligible cost.
- **Data starts at 1 MiB** (partition and SSD erase-block alignment, and room
  reserved for future structures). Segment headers are one 4 KiB block.
- **Data chunks record their owner** (inode, file offset), so `fsck` and GC
  can identify a chunk from a segment scan alone.

### 7.1 Why not update compressed data in place

Compressed sizes change with content. Rewriting a chunk in place usually does
not fit in the old space, and a crash halfway through corrupts it. Therefore
Nanoer **never overwrites live data**.

### 7.2 Copy-on-write (CoW)

Changes are written to **new** locations, and pointers are then switched
atomically to the new version. The old version stays valid until the switch
completes, so a crash leaves either the old or the new state, never a mix.
Used by ZFS, Btrfs and APFS. CoW also makes **snapshots** cheap: keep the old
pointers.

### 7.3 Log-structured storage

All writes are **appended** to large sequential **segments** (Sprite LFS,
Rosenblum & Ousterhout, 1991; F2FS on flash). Benefits:

- Sequential writes are the fastest pattern on HDDs and SSDs, and the easiest
  on flash wear.
- Natural CoW: nothing is overwritten.
- Crash recovery: replay the log from the last checkpoint.

Cost: space from deleted or overwritten data must be reclaimed by **garbage
collection** (§7.6).

### 7.4 Journaling and write-ahead logging (WAL)

Before metadata changes are applied, a description of them is written to a
**journal** and flushed. After a crash, completed journal entries are
replayed and incomplete ones are discarded (ext4 jbd2, XFS, databases).
Nanoer's log is its journal. A **checkpoint** periodically records a
consistent snapshot of metadata so replay stays short.

### 7.5 Metadata: B+trees

Filesystems need sorted, searchable maps: inode → attributes,
(inode, offset) → chunk location, directory → entries, hash → chunk (dedup).
A **B+tree** keeps keys sorted in wide nodes (one node = one disk block), so
lookups take only a few disk reads even with billions of entries. A
**CoW B+tree** ("shadowing", Ohad Rodeh, 2008, the basis of Btrfs) changes a
leaf by writing a new leaf and new copies of its parents up to a new root,
then atomically switches the root pointer.

**Why not an embedded database?** SQLite (public domain) and LMDB (OpenLDAP
licence) are excellent, but stage 3 needs the same format inside the kernel,
where neither can run. RocksDB (LSM tree, Apache-2.0/GPL-2.0) has the same
problem and adds compaction overhead. Nanoer therefore owns its metadata
format. Stage 1 may start with in-memory metadata plus checkpoints and add the
on-disk B+tree once the rest works (see the stage 1 plan).

### 7.6 Garbage collection (GC) and compaction

Overwrites and deletes leave **dead** chunks inside segments. GC picks
segments with a low live-data fraction, copies their live chunks into a new
segment, and frees the old one. The selection policy matters: "greedy"
(least live data) versus "cost-benefit" (also weighs data age, from Sprite
LFS). GC must respect dedup reference counts and run when the disk is idle.

### 7.7 Hot/cold tiering (Nanoer-specific)

Since GC already rewrites data, it can **recompress** it at the same time:

- New writes use a fast codec (zstd level 1–3, or LZ4) to keep write latency
  low.
- When GC moves data that has not been read for a long time (cold), it
  recompresses it with zstd 19 or LZMA and, optionally, larger chunks.
- Because zstd decompresses at the same speed at every level, most cold data
  even reads back as fast as before.

Btrfs and ZFS can only do this through a manual rewrite (defragment or
recompress). Doing it automatically is one of Nanoer's intended advantages.

### 7.8 Small files

A 300-byte file in a 4 KiB block wastes 93 %. Solutions: **inline** tiny
files inside metadata (as Btrfs does), and **pack** many small files into one
shared compressed chunk, ideally with a zstd dictionary trained on similar
files.

### 7.9 Crash consistency and `fsync`

POSIX promises durability only after `fsync`/`fdatasync`. The classic way
(ext4's journal) is two flushes per commit: write the data, flush, write the
commit record, flush, so the disk cannot reorder the commit record ahead of
the data it describes.

**Nanoer needs only one flush per commit**, thanks to *prefix consistency*
(§7.10): data chunks are appended before the transaction that refers to
them, and replay stops at the first record that is not intact. If the disk
persisted the transaction but not an earlier data chunk, replay stops at
the missing chunk and never reaches the transaction. A crash therefore
always recovers a state that really existed, even if the disk reorders
writes. One `fdatasync` at the end makes the whole commit durable.

### 7.10 How the M2 engine works

- **One log for everything.** Data chunks, metadata transactions,
  checkpoint parts and segment seals are appended to a single log that runs
  through segments, linked by **seals** ("continue in segment N, sequence
  S"). Replay follows seals only into a segment whose header is intact with
  exactly that sequence number.
- **Physical operations, one `apply()`.** Metadata changes are recorded as
  small physical operations (put inode, set entry, set chunk, …; nine in
  all, `docs/format.md` §8). The live filesystem and log replay run the
  same `Metadata::apply()`, so a recovered state is, by construction, the
  state that was live. The filesystem layer turns POSIX semantics (rename
  rules, link counts, …) into operations; replay never re-runs that logic.
- **Commits.** Operations take effect in memory at once and queue up; file
  data is kept as whole dirty chunks. A commit compresses the dirty chunks
  in parallel, appends them, appends one transaction (queued operations,
  then the new chunk locations, then the attributes of every changed
  inode), and flushes once. Commits happen on `fsync`, every 5 seconds,
  when 64 MiB of dirty data builds up, or when the queued operations grow
  past half of what a transaction may hold. **Every commit is durable**,
  and `fsync` on one file commits everything queued before it (simpler
  than per-file tracking, and a stronger guarantee).
- **Holes.** A chunk that is entirely zeros is never written: it becomes a
  hole. Files are cut on a fixed grid of `chunk_size`, so a write touches
  only the chunks it overlaps.
- **Checkpoints** (every 256 MiB of log, every 5 minutes if anything was
  written, and at unmount) write the whole metadata as one or more parts and
  point the superblock at them. In M2 the metadata is in memory, so a
  checkpoint rewrites all of it; M6's on-disk B+tree removes that cost.
- **Mount** scans every segment header (sequence numbers must exceed all of
  them), reads the checkpoint, replays the transactions after it, frees
  orphans (files deleted while open when the system went down), seals the
  old log *at* its end (never appending after a possibly torn tail) and
  continues in a new segment.
- **Segment reuse.** Segments with no live data, older than the newest
  checkpoint, are released when a checkpoint is written, and become free
  only after the next one, because the previous superblock may still need
  them if the newest superblock write tears. At mount they are free at
  once: the superblock just read is durable and stays valid for two more
  checkpoints. A reused segment gets a new, higher sequence number, so its
  old records can never be replayed.
- **Running out of space.** Each write is checked against free space before
  anything changes, conservatively (dirty data counted uncompressed at a
  full chunk each). If the check fails, Nanoer commits, writes two
  checkpoints to free released segments, and checks again before returning
  `ENOSPC`. A **reserve** of segments (3, plus room for 1.5× the last
  checkpoint) is never used by normal writes, so a checkpoint and the next
  mount always have room. **Deletes, rmdir, removing xattrs and shrinking
  files skip the check**: they are how a full filesystem gets space back,
  and their transactions are tiny. If a filesystem is so full that mount
  cannot start a new segment, it mounts **read-only** with a warning rather
  than refusing.
- **If a write to the store fails** mid-commit, memory holds changes the log
  does not. The filesystem then refuses further changes (`EIO`), like ext4's
  `errors=remount-ro`, rather than let memory and disk drift apart.
- **Limit found in M2:** without copying garbage collection (M4), only
  segments whose data is *entirely* dead are reclaimed. Random rewrites leave
  a little live data in many segments, so space runs out long before the
  data fills it: in the model-based test, about 0.5 MB of live data held 16
  of 23 segments (4 MiB each). Workloads that write files once (copies,
  untar, builds) are unaffected; rewrite-heavy ones (databases, VM images)
  need M4.

---

## 8. Caching: the safe space and read-ahead

### 8.1 The safe space

A memory budget for **decompressed chunks**. Because a single read decompresses
a whole chunk, keeping recent chunks avoids decompressing the same chunk again
for neighbouring reads.

### 8.2 Replacement policies

- **LRU** (least recently used): simple and good for most workloads. Weakness:
  one large scan (a backup, a virus scan, file indexing) flushes everything
  useful.
- **2Q, CLOCK-Pro, ARC** (Adaptive Replacement Cache, Megiddo & Modha, IBM,
  2003; used by ZFS): keep separate "seen once" and "seen repeatedly" lists,
  so scans cannot evict hot data.
- **Nanoer plan:** a sharded LRU first (one lock per shard, so many threads do
  not fight over a single lock), then 2Q or ARC once benchmarks show scan
  pollution.
- **In M2:** a 16-shard LRU, 256 MiB by default (`-o safe_space=`). Chunks are
  keyed by record location **and** the segment's sequence number: when a
  freed segment is reused, the same location holds different data under a
  new sequence number, so a stale entry can never be returned and the cache
  needs no invalidation.

### 8.3 Interaction with the kernel page cache

The kernel already caches file pages that FUSE returns. Decompressed data may
therefore sit in **both** the page cache and the safe space (double caching).
The safe space only has to cover the gap: a chunk is larger than one kernel
read request. Hence it can stay small (a few MiB to a few hundred MiB) and is
user-configurable.

### 8.4 Read-ahead and prefetch

Sequential readers (video, game level loading, file copies) are detected per
open file. The next chunk(s) are decompressed in the background on a thread
pool before they are requested. Linux has its own read-ahead (default
`read_ahead_kb` = 128 KiB); Nanoer's operates at chunk granularity on top of
it.

---

## 9. Linux internals Nanoer depends on

### 9.1 VFS, inodes, dentries

The **Virtual File System** is the kernel layer every filesystem plugs into.
An **inode** is a file's identity and attributes (size, permissions, owner,
timestamps, link count). A **dentry** maps a name in a directory to an inode.
Hard links are several dentries pointing to one inode. Nanoer must implement
all of this faithfully: permissions, ownership, timestamps, symlinks, hard
links, extended attributes (xattrs; SELinux and capabilities live there),
`rename` semantics, sparse files and `mmap`.

### 9.2 FUSE (Filesystem in Userspace)

Merged into Linux 2.6.14 (2005), by Miklos Szeredi.

```
program ──read()──► kernel VFS ──► fuse kernel module ──► /dev/fuse ──► nanoerfs (user space, C++)
                                                                         │ decompress chunk
program ◄─────────── data ◄──────── kernel page cache ◄──────────────────┘
```

- **libfuse 3** offers a *high-level* (path-based) API and a *low-level*
  (inode-based) API. **Nanoer uses the low-level API**: it maps directly onto
  inode-based metadata, avoids path lookups on every call, and gives control
  over replies and caching.
- **Writeback cache** (`FUSE_CAP_WRITEBACK_CACHE`): the kernel batches small
  writes into larger ones before sending them, which is essential for
  performance.
- Request size: recent kernels allow up to 1 MiB per request (`max_pages`).
  Larger requests mean fewer user/kernel round trips.
- **FUSE passthrough** (Linux 6.9) lets a FUSE filesystem hand a plain backing
  file to the kernel. It does not apply to compressed data, but is useful for
  raw-stored files.
- **FUSE over io_uring** (Linux 6.14) cuts per-request overhead and is a
  future performance option.
- **Why FUSE for stages 1–2:** safe (a bug crashes nanoerfs, not the kernel),
  allows C++, quick to develop and debug. **Cost:** context switches and data
  copies make it slower than in-kernel filesystems, which is the reason for
  stage 3.

**How `nanoerfs` uses FUSE (M2):**

- libfuse 3.12+ low-level API (`FUSE_USE_VERSION 312`), multi-threaded loop
  (up to 16 threads), writeback cache on, 1 MiB requests and read-ahead.
- **`default_permissions`**: the kernel checks permissions from mode, uid and
  gid, so Nanoer does not reimplement access checks.
- **Attribute and name cache timeouts of 60 s.** Every change goes through
  the one mount (the store is locked against other writers), and the kernel
  updates its own caches for those changes, so they cannot go stale. Going
  from 1 s to 60 s made untar ~15 % and a warm `grep -r` ~17 % faster.
- **No `lseek` operation.** With the writeback cache, freshly written data
  can still be in the kernel's page cache when `SEEK_DATA`/`SEEK_HOLE`
  arrives, so Nanoer would report a hole where there is data, and tools
  that skip holes (`cp`, `tar`) would lose it. Without the operation the
  kernel treats the whole file as data: holes are not reported, which POSIX
  allows, and data is never skipped. Found by the POSIX suite on a real mount.
- **Lookup counting.** Every successful lookup, create and link counts as a
  kernel reference until `forget`. `readdirplus` counts every entry it
  returns except `.` and `..`, so it checks that an entry fits in the reply
  buffer *before* looking it up. A deleted file is freed only when it has no
  links, no kernel references and no open handles.
- **`readdir` serves a snapshot** taken at `opendir`, so entries added or
  removed meanwhile can never make a listing skip or repeat a name.
- **Threads after `fork`.** `nanoerfs` daemonizes (forks) after mounting.
  Threads do not survive `fork()`, so the compression pool and the commit
  thread start only afterwards.
- **`mount.nanoer`** is the same program; `mount -t nanoer IMAGE DIR` and
  `/etc/fstab` work. fstab-only options (`defaults`, `nofail`, `_netdev`,
  `x-*`, atime options) are accepted and ignored.
- **atime** is never updated on reads (decision 3 of the M2 plan).
- **`st_blocks`** reports *uncompressed* data, as Btrfs does. Reporting the
  compressed size would make tools such as `cp` and `tar` think files are
  sparse. Compressed sizes are shown by `nanoer info`.

### 9.3 Why the kernel driver cannot be C++

The Linux kernel accepts C and, since Linux 6.1 (2022), **Rust**. C++ is
excluded: it needs a runtime (exceptions, RTTI, global constructors) the
kernel does not provide, and the kernel community has rejected it. Therefore
the **on-disk format must be specified independently of the C++ code**
(`docs/format.md`), so that a C or Rust kernel driver can implement it later.

### 9.4 Booting from Nanoer (stage 2)

- **initramfs:** a small in-memory root filesystem the kernel starts first. It
  loads drivers and mounts the real root. Nanoer adds a hook there to start
  nanoerfs and mount the system image.
- **overlayfs** (in the kernel since 3.18): stacks a writable *upper* layer
  over a read-only *lower* layer. Used by Docker and live USBs. A candidate
  for combining the read-only system image with the writable layer.
- **Image-based OSes** do this today: SteamOS, Fedora Silverblue, Android
  (EROFS system partitions).

### 9.5 Memory: demand paging, and why Nanoer does not reduce RAM

A common assumption is that loading a chunk instead of the whole program
saves RAM. It does not, because Linux already loads programs lazily:

- **Demand paging:** executables and libraries are memory-mapped (`mmap`).
  A 4 KiB page is read into RAM only when the program first touches it, and
  unused clean pages can be dropped under memory pressure and re-read later.
  The whole program is never loaded at once, with or without Nanoer.
- **A program's RAM use is mostly its own data:** heap allocations, decoded
  textures, levels, documents, browser tabs. Storage format does not change
  it.
- **Nanoer adds RAM:** the safe space (configurable), metadata, and possible
  double caching with the kernel page cache (§8.3).
- **What Nanoer does save:** disk space, and fewer bytes read from disk. On
  slow storage (HDD, SD card, USB) that can make loading faster, at the cost
  of CPU time for decompression.

**Technologies that do reduce RAM use** compress memory pages themselves,
separately from the filesystem:

- **zram:** a compressed block device in RAM, used as swap. Fedora enables it
  by default (since Fedora 33).
- **zswap:** a compressed in-RAM cache in front of a swap device.
- Windows and macOS have built-in memory compression too.

They typically fit 2–3× more inactive memory into the same RAM *(approx.)*,
at some CPU cost. A later Nanoer stage could configure them with good
defaults, but they are not part of the Nanoer filesystem.

---

## 10. Existing systems and how Nanoer differs

| System | Type | Compression | Dedup | Licence | Notes |
|---|---|---|---|---|---|
| **Btrfs** | in-kernel CoW filesystem, read-write | zlib, LZO, zstd (since 4.14); 128 KiB extents | out-of-band only (`duperemove`) | GPL-2.0 | Fedora default with zstd:1 |
| **ZFS / OpenZFS** | CoW filesystem + volume manager | LZ4, zstd, gzip; per record (default 128 KiB, up to 16 MiB) | in-line, RAM-hungry | CDDL (cannot be merged into Linux) | ARC cache |
| **EROFS** | in-kernel, read-only (since 5.4) | LZ4, LZMA, DEFLATE, zstd; fixed-size output | yes | GPL-2.0 | Android system partitions |
| **squashfs** | in-kernel, read-only | gzip, LZO, xz, LZ4, zstd; blocks up to 1 MiB | duplicate files | GPL-2.0 | live USBs, snaps |
| **DwarFS** | FUSE, read-only, C++ | zstd, LZMA, Brotli, … | strong (segmenting) | GPL-3.0 | extreme ratios on similar files |
| **dm-vdo** | kernel block layer (since 6.9) | LZ4 | in-line, block-level | GPL-2.0 | under any filesystem |
| NTFS / Windows CompactOS (WOF) | Windows | LZNT1; XPRESS4K–16K, LZX | — | proprietary | `compact /exe:lzx`, popular for games |
| APFS (decmpfs) | macOS | zlib, LZVN, LZFSE | — | proprietary | system files |

**Intended advantages for Nanoer** (to be proven by benchmarks, not assumed):

1. **Automatic hot/cold tiering:** fast writes, maximum compression for cold
   data, with no manual recompression (§7.7).
2. **Combined dedup and compression** with a user-controlled memory budget
   (the safe space).
3. **Per-folder policies:** for example, games use LZ4 and archives use LZMA.
4. **Works without reformatting** (stage 1): the store lives in an image file
   on any existing filesystem.
5. One format spanning read-write folders (stage 1), a whole-OS image
   (stage 2) and a kernel driver (stage 3).

Yardstick: **Btrfs with zstd compression.** If Nanoer cannot beat it on space,
speed or features for some realistic workload, the design is reconsidered.

---

## 11. Tech stack and reasons

| Component | Choice | Licence | Why |
|---|---|---|---|
| Language (engine, tools) | **C++20** | — | speed, control over memory and threads, mature libraries; `std::span`, concepts, `<bit>`, `std::jthread` |
| Language (future kernel driver) | C or Rust | — | the only languages the kernel accepts (§9.3) |
| Compilers | GCC 13+, Clang 18+ | — | both in CI; Clang for sanitizers and libFuzzer |
| Build system | **CMake** 3.20+ | BSD-3 | industry standard; IDE support; `compile_commands.json` |
| FUSE | **libfuse 3** (low-level API) | LGPL-2.1 | the standard; LGPL allows linking from Apache-2.0 code |
| Codecs | **zstd**, **LZ4**, **liblzma** | BSD/GPL-2.0, BSD-2, 0BSD | §3 |
| Hashes | **xxHash** (XXH3), **BLAKE3**, CRC-32C | BSD-2, CC0/Apache-2.0, own code | §5 |
| Unit tests | **GoogleTest** | BSD-3 | widely used, good C++ support |
| Fuzzing | libFuzzer (LLVM) | Apache-2.0 with LLVM exception | fuzz the format parser and codecs with random and corrupt input |
| Sanitizers | ASan, UBSan, TSan | — | catch memory errors, undefined behaviour and data races |
| POSIX conformance | pjdfstest; fsx | test tools only, not shipped | standard filesystem test suites |
| Benchmarks | fio, plus real workloads (untar, compile, game loads) | GPL-2.0 (tool only) | standard I/O benchmark |
| CI | GitHub Actions, Ubuntu runners | — | FUSE available on hosted runners |

Dependencies come from distribution packages located through `pkg-config`,
at pinned minimum versions. Every dependency and its licence is listed here
(supply-chain hygiene, §3.3).

**Versions in use** (Ubuntu 24.04, 2026-10-24): zstd 1.5.5, LZ4 1.9.4,
XZ Utils / liblzma 5.4.5 (from before the 5.6.0/5.6.1 backdoor), xxHash
0.8.2, libfuse 3.14.0 (used from M2), GoogleTest 1.14.0, CMake 3.28,
GCC 13.3, Clang 18. Minimum versions required by the build: zstd 1.4.5,
LZ4 1.9.0, liblzma 5.2.0, xxHash 0.8.0.

**BLAKE3** is vendored in `third_party/blake3`: the official C
implementation 1.3.1, taken from Ubuntu's `librust-blake3-dev` package
(signed archive) because GitHub downloads were blocked in the build
environment. `SHA256SUMS` pins every file, and CI checks it. The portable,
SSE2, SSE4.1, AVX2, AVX-512 and NEON code paths are built with runtime CPU
detection.

**Error handling:** the engine throws `nanoer::Error`, which carries a
category (I/O, corrupt, invalid argument, unsupported, no space, busy) and
the system errno. The FUSE adapter converts it to an errno at the boundary.
Corruption is reported as `EUCLEAN`, as Btrfs and ext4 do. Exceptions were
chosen over error-code returns because they keep the many layers of
storage code readable, and C++ is never used in the kernel anyway (§9.3).

**CRC-32C** is Nanoer's own code: the SSE4.2 `crc32` instruction on x86-64
or the ARMv8 CRC instructions, detected at run time, with a slicing-by-8
table fallback that processes 8 bytes per step.

---

## 12. Testing and verification techniques

- **Unit tests:** codecs round-trip, checksums, format encode/decode, cache
  eviction, allocator.
- **Model-based testing:** run random operation sequences against Nanoer and
  an in-memory reference model, and compare results after every step.
- **fsx (file system exerciser):** random reads, writes, truncates and mmaps
  with data verification. It finds subtle data-corruption bugs.
- **pjdfstest:** POSIX semantics (permissions, rename, links, …).
- **Crash testing:** a test backing store records every write and flush, then
  simulates a power cut at each point by discarding unflushed writes,
  remounts, and checks the filesystem is consistent and synced data is
  present. (Same idea as Linux's `dm-log-writes` and the CrashMonkey
  research tool.)
- **Fuzzing:** feed corrupted or random images to the mounter and `fsck`.
  They must reject them cleanly, never crash or return wrong data.
- **Sanitizers** on every CI run.
- **In M1:** 59 unit tests (GoogleTest), an end-to-end test of the tools,
  ASan + UBSan builds with GCC and Clang, a ThreadSanitizer build, and three
  libFuzzer targets (decompressors, codec round trips, on-disk structure
  parsing) with seed inputs in `fuzz/seeds`. **Lesson:** fuzz coverage was
  near zero until the library itself was compiled with
  `-fsanitize=fuzzer-no-link`. Instrumenting only the fuzzer's entry file
  is not enough.
- **In M2:** 103 unit tests plus an end-to-end test of the tools, including:
  - a **model-based test** (4 seeds × 1,500 random operations against a
    simple reference model, with remounts and simulated power cuts mixed
    in; a crash must roll the filesystem back to exactly the last sync);
  - journal tests where unflushed writes are randomly kept, dropped or torn,
    and the recovered state must equal one that really existed;
  - tests for running out of space and recovering from it.

  On a **real FUSE mount**: a 15-test POSIX suite (`tests/integration`),
  `nanoer-fsx` (random reads, writes, truncates, hole punches, mmap reads
  and writes, checked byte by byte against a shadow copy), and a
  persistence test (copy a tree, remount, `diff -r`). **pjdfstest** and
  **xfstests' fsx** run in CI only: neither is packaged for Ubuntu, and
  GitHub was not reachable from the development environment.
- **Lesson:** a test filter (`Fs*`) silently did not match the model test's
  name (`Seeds/FsModel...`), so it was believed to pass before it had ever
  run. Check that a test actually ran (ctest's count, or `--gtest_list_tests`)
  before relying on its result.
- **Lesson:** headers include each other differently between standard
  library versions. Code that used `std::min({...})` without `<algorithm>`
  built against libstdc++ 13 but not against the CI runners' libstdc++ 14.
  Include what you use; the development environment now also has
  libstdc++ 14 installed, so Clang builds there match CI.
- **Lesson:** a test that copies a tree and compares it must use a tree
  nothing else is writing to. Copying the source tree failed spuriously
  while a build was running inside it.
- **Benchmarks** compared with ext4 (no compression) and Btrfs + zstd: space
  used, throughput, latency, CPU time.

---

## 13. Licensing

**Decision: Nanoer is licensed under Apache-2.0** (2026-10-24). See `LICENSE`.

- **What Apache-2.0 allows:** use, modification and distribution, including
  commercially and in closed products, provided the licence and copyright
  notices are kept and modified files are marked as changed.
- **Patent grant:** every contributor grants users a licence to any of their
  patents the contribution uses. The grant terminates for anyone who sues
  claiming the project infringes a patent. This is the main reason to prefer
  it over MIT.
- **Why not GPL:** a permissive licence makes adoption by distributions,
  companies and other projects easier, and Nanoer's value grows with
  adoption.
- **Kernel caveat (stage 3):** kernel modules must be GPL-compatible. The
  kernel driver will be licensed **"Dual MIT/GPL"** (or GPL-2.0), declared
  with `MODULE_LICENSE("Dual MIT/GPL")`. Apache-2.0 code cannot simply be
  copied into the kernel, because Apache-2.0 is incompatible with GPL-2.0.
  This is another reason the on-disk format is specified separately (§9.3).
- **Dependency licences:** all compatible with Apache-2.0 (§11). libfuse is
  LGPL-2.1: linking to it dynamically is fine. Modifying libfuse itself would
  make those changes LGPL.
- **GPL-licensed tools** (fio, xfstests' fsx) are used for testing only and
  never shipped or linked.

*This is general information, not legal advice.*

---

## 14. Measurements

### 2026-10-23: compressibility of Linux system files

A 290 MiB sample of `/usr/bin`, `/usr/lib`, `/usr/share` (Ubuntu 24.04
container), compressed in independent chunks. Stored-raw fallback applied.

| Chunk size | zlib level 6 | LZMA preset 1 |
|---|---|---|
| 128 KiB | 35.9 % | 31.5 % |
| 4 MiB | 35.5 % | 29.9 % |

**Conclusions:** system files compress well, so 800 → 500 GB is achievable
for OS and application data. (This sample was later found to be skewed
toward a few large files; the 2026-10-24 C++ benchmark is the reference.) Bigger chunks help only slightly, so
128 KiB is a sound default for writable data.

### 2026-10-24: codec benchmark on Linux system files (C++ engine)

`nanoer bench --max-size 256M --per-file 1M /usr/bin /usr/lib /usr/share
/usr/include`: 256 MiB from 4,158 files, 128 KiB chunks, one thread,
incompressible chunks stored raw. Release build, Intel Xeon @ 2.80 GHz
(cloud VM, 4 cores), Ubuntu 24.04.

| Codec | Level | Stored | Compress MB/s | Decompress MB/s |
|---|---:|---:|---:|---:|
| lz4 | 0 | 51.9 % | 365 | 3218 |
| lz4 | 9 | 45.2 % | 32 | 3373 |
| zstd | 1 | 42.5 % | 248 | 1099 |
| zstd | 3 | 41.0 % | 188 | 1055 |
| zstd | 9 | 39.0 % | 51 | 976 |
| zstd | 19 | 37.2 % | 2 | 782 |
| lzma | 1 | 36.7 % | 13 | 41 |
| lzma | 6 | 35.5 % | 4 | 42 |

**Conclusions:**
- Even the fastest setting (LZ4) stores OS files at about half their size,
  and the default for new writes (zstd 1, 42.5 %) is well below the 62.5 %
  target.
- **Hot/cold tiering is confirmed worthwhile:** recompressing cold data from
  zstd 1 to zstd 19 saves another ~5 points of the original size, and still
  decompresses at ~780 MB/s.
- **LZMA is a poor default even for cold data:** only ~1.7 points smaller
  than zstd 19, but decompresses ~19× slower (42 vs 782 MB/s). Keep it for
  explicit "max" policies only.
- The earlier Python measurement (§14, 2026-10-23: 31.5–35.9 %) used a
  different sample (the first 300 MB of a directory walk, dominated by a few
  large files) and is not directly comparable. This one, spread over many
  files, is the reference.

### 2026-10-24: M2, the mounted filesystem against ext4

Release build; image file on the same ext4 disk; 4-core VM; single runs,
so take differences under ~20 % as noise.

**Space.** The same content (`/usr/include`, Python 3.12's standard library,
Nanoer's git repository and its build output), 95.3 MiB logical, 6,317
files:

| | Disk used |
|---|---:|
| ext4 | 110 MB |
| Nanoer (actual blocks of the image file) | **32 MB** |
| Nanoer stored data | 29.0 MiB = **30.5 %** of logical |

That is **71 % less disk** than ext4 for OS-like content, well past the
37.5 % the 800 → 500 GB goal needs.

**Time** (seconds; "cold" means after a remount, so the kernel's page cache
is empty):

| Workload | ext4 | Nanoer |
|---|---:|---:|
| untar 104 MB of headers + Python | 2.4 | 5.7 |
| copy 512 MB incompressible | 1.1 | 2.4 |
| copy a 60 s 720p H.264 video | 0.05 | 0.05 |
| `git clone` (local) | 0.11 | 0.39 |
| `cmake` configure | 1.1 | 1.2 |
| compile Nanoer (4 jobs) | 8.0 | 7.9 |
| `git status` | 0.01 | 0.03 |
| `find` all files | 0.02 | 0.13 |
| `grep -r`, warm | 0.10 | 0.78 |
| read 512 MB, cold | 0.46 | 0.77 |
| decode the video, cold | 2.5 | 2.6 |
| `grep -r`, cold | 0.74 | 1.7 |
| `tar` the tree, cold | 0.24 | 0.88 |
| `rm -rf` everything | 2.1 | 1.1 |

**Conclusions:**
- **CPU-bound work is unaffected** (compiling, video decoding), and the
  compile *from* a compressed filesystem is as fast as from ext4.
- **Metadata-heavy work pays the FUSE price:** 2–9× slower for many small
  files (untar, find, grep). Each file costs several user/kernel round
  trips. This is M5's tuning target and ultimately the reason for the stage
  3 kernel driver.
- Large sequential I/O is about 2× slower; cold reads run at ~660 MB/s.

### 2026-10-23: media and raw video (Python prototype)

| Input | Result |
|---|---|
| H.264 MP4, 9.6 MiB, 1 MiB chunks, zlib / bz2 / lzma | 99.8–99.9 % (9 of 10 chunks stored raw) |
| Uncompressed AVI (synthetic test pattern), 16.5 MiB, lzma | 0.8 % (unrealistically good; real raw footage compresses far less) |

**Conclusion:** already-encoded media does not compress. The stored-raw
fallback is essential.

---

## 15. Decision log

Newest last. Record the decision, the date, the reason and the alternatives
rejected.

| Date | Decision | Reason | Alternatives rejected |
|---|---|---|---|
| 2026-10-23 | Independent chunks + index | random access for live extraction | single stream (no random access) |
| 2026-10-23 | Store incompressible chunks raw | media does not compress; avoid growth and CPU waste | always compress |
| 2026-10-23 | Checksum uncompressed data, verify after decompression | catch disk errors and codec bugs; never return wrong data | checksum compressed data only |
| 2026-10-23 | Plan and get approval before multistep work | user's rule, see `CLAUDE.md` | — |
| 2026-10-23 | Scope: OS-level transparent compression, Linux first | user goal: 800 GB → 500 GB | per-file archive tool only |
| 2026-10-23 | C++20 engine in user space via FUSE | speed; the kernel cannot use C++; FUSE is safe to develop on | kernel module first (too risky and slow to develop) |
| 2026-10-23 | Specify the on-disk format separately from the code | a future kernel driver in C or Rust must implement it | format defined only by the C++ code |
| 2026-10-24 | Licence: Apache-2.0; future kernel module Dual MIT/GPL | adoption plus patent grant; kernel requires GPL compatibility | MIT (no patent grant), GPL-2.0 (limits adoption), proprietary |
| 2026-10-24 | Remove the Python prototype | superseded by the C++ engine; kept in git history | keep as reference implementation |
| 2026-10-24 | Copy-on-write, log-structured storage | compressed sizes vary, so in-place updates are unsafe; gives crash safety; GC enables tiering | in-place updates; journaling over an in-place layout |
| 2026-10-24 | Own metadata format (CoW B+tree), not an embedded database | must be implementable in the kernel later | SQLite, LMDB, RocksDB |
| 2026-10-24 | Default chunk ~128 KiB for writable data | measured: larger chunks gain only ~1.5 points; smaller chunks are cheaper to rewrite | 4–50 MiB chunks |
| 2026-10-24 | BLAKE3 for dedup identity, XXH3 for integrity, CRC-32C for metadata | collision resistance where correctness depends on it, speed elsewhere | SHA-256 (slower), XXH3 for dedup (unsafe) |
| 2026-10-24 | Stage 1 plan approved with the recommended defaults: image file **and** block device; metadata in memory first (M2), on-disk B+tree in M6; dedup in M4; 128 KiB chunks + zstd level 1 for new writes; GitHub Actions CI; Ubuntu packages + vendored BLAKE3 | user approval of `docs/STAGE1_PLAN.md` | see plan §9 |
| 2026-10-24 | Engine errors are C++ exceptions (`nanoer::Error`), mapped to errno at the FUSE boundary; corruption is `EUCLEAN` | readable layered storage code; matches Btrfs/ext4 reporting | error-code returns everywhere, `std::expected` (C++23) |
| 2026-10-24 | Superblock written to slot `generation mod 2`; never fall back past an unsupported newest superblock | torn writes never lose the filesystem; no silent data loss on version skew | fixed primary + backup copy |
| 2026-10-24 | Segment headers carry UUID + sequence number; mkfs does not clear segments | free segments are recognised without clearing; instant mkfs | zeroing the whole disk |
| 2026-10-24 | Every record carries its segment's sequence number, a header CRC-32C and a payload CRC-32C | rejects stale records in reused segments and torn writes, without decompressing | rely on XXH3 of decompressed data only |
| 2026-10-24 | Records 8-byte aligned; data starts at 1 MiB | 4 KiB alignment would waste >5 % on compressed chunks | 4 KiB alignment, byte alignment |
| 2026-10-24 | LZ4 block format; zstd frame; .xz (check None, LZMA2 only) for lzma, with the dictionary shrunk to the chunk | minimal overhead; all decodable by in-kernel decoders; avoids 8 MiB allocation per chunk | LZ4 frames, raw LZMA2, .lzma |
| 2026-10-24 | Own CRC-32C (hardware + slicing-by-8 fallback) | no extra dependency; hardware speed | a CRC library |
| 2026-10-24 | BLAKE3 1.3.1 vendored from Ubuntu's package with SHA-256 pins | GitHub downloads blocked; signed source; reproducible | fetching at build time |
| 2026-10-24 | Exclusive `flock` on the backing store | two writers would destroy a store | no locking |
| 2026-10-24 | LZMA only for explicit "max" policies, not as the cold-data default | measured: ~1.7 points smaller than zstd 19 but ~19× slower to read | LZMA for all cold data |
| 2026-10-24 | M2 plan approved with the recommended answers: reclaim entirely-dead segments in M2; 5 s commit interval; no atime updates on reads; POSIX ACLs stored, not enforced; one metadata lock; pjdfstest and xfstests fsx in CI only; mkfs writes an initial checkpoint; `nanoerfs` / `mount -t nanoer` | user approval of `docs/M2_PLAN.md` | see plan §12 |
| 2026-10-24 | One log for data, transactions, checkpoints and seals; replay stops at the first damaged record and enters segments only through intact seals | prefix consistency: crash recovery always yields a state that existed, with one flush per commit | separate data and metadata areas; two flushes per commit |
| 2026-10-24 | Physical metadata operations, one `apply()` for live use and replay | replay cannot diverge from what was live | logging POSIX-level operations and re-running their logic |
| 2026-10-24 | Every commit is durable; `fsync` commits everything queued | simple, and a stronger guarantee than POSIX | per-file fsync tracking |
| 2026-10-24 | Resume after mount by sealing the old log at its end, in a new segment | never append after a possibly torn tail | appending in place; a checkpoint at every mount |
| 2026-10-24 | Sequence numbers above every segment header found, not just the superblock's | a segment started but never linked must not share a number with a new one | trusting `next_segment_seq` |
| 2026-10-24 | Free dead segments at mount; accept a reused one in replay only if the snapshot listed it without live data | a full filesystem must still mount; reuse stays detectable as corruption otherwise | waiting two checkpoints after every mount |
| 2026-10-24 | Reserve 3 segments + 1.5× checkpoint size; deletes skip the space check; mount read-only if no segment is free | deleting must always work on a full filesystem; never refuse to mount over space | a fixed 2-segment reserve (tests found it too small) |
| 2026-10-24 | All-zero chunks are stored as holes | zeros cost nothing to store | storing compressed zeros |
| 2026-10-24 | `st_blocks` reports uncompressed bytes, as Btrfs does | compressed values make tools treat files as sparse | compressed size in `st_blocks` |
| 2026-10-24 | No FUSE `lseek`; the kernel then treats files as all data | with the writeback cache Nanoer can miss data still in the page cache, and hole-skipping tools would lose it | answering `SEEK_DATA`/`SEEK_HOLE` from Nanoer's metadata |
| 2026-10-24 | 60 s attribute and name cache timeouts | single writer through one mount: caches cannot go stale; measured ~15 % faster on small-file work | 1 s (libfuse examples) |
| 2026-10-24 | `readdir` from a snapshot taken at `opendir` | concurrent changes cannot make a listing skip or repeat names | live iteration with name cookies |
| 2026-10-24 | BSD semantics on set-group-ID directories (new entries take the group; subdirectories inherit the bit) | what Linux filesystems do; the kernel leaves it to the filesystem under FUSE | ignoring set-group-ID |

---

## 16. History: the Python prototype (format v1)

Built 2026-10-23, removed 2026-10-24 (recover it from git history, commit
"Add Nanoer live extraction"). It proved the live-extraction idea:

- **Format v1** (`.nnr`): header (magic `NANOER\x1a\n`, version, codec, chunk
  size, file name) → chunks → index (offset, compressed length, CRC-32,
  stored flag per chunk) → trailer (index offset, original size, chunk
  count, end magic). The index sits at the end so archives can be written in
  one pass.
- **Reader:** an LRU window of decompressed chunks capped by the safe space,
  background prefetch of the next chunk, thread-safe sharing between readers,
  and a seekable file object.
- **HTTP server** with `Range` support, so video players could play and seek
  in archives. Verified with ffmpeg: a seek skipped all intermediate chunks,
  and the decoded output was identical to the original.

**Lessons carried forward:**
- Players read container headers (start of file) and indexes (end of file)
  repeatedly. A two-chunk window thrashed on them. The cache needs room for
  such hot spots, or a scan-resistant policy (§8.2).
- The stored-raw fallback and per-chunk checksums were essential.

---

## 17. Glossary

- **Chunk:** the unit of compression. Decompressed as a whole.
- **Segment:** a large append-only region of the backing store holding many
  compressed chunks.
- **Safe space:** the memory budget for decompressed chunks.
- **Live extraction:** decompressing only the chunks a read needs, on demand.
- **Read / write amplification:** extra bytes read or written, beyond what the
  program asked for.
- **CoW:** copy-on-write. Never overwrite; write anew and switch pointers.
- **GC:** garbage collection. Reclaiming space from dead chunks.
- **Hot / cold data:** frequently / rarely accessed data.
- **Inode:** a file's identity and attributes. **Dentry:** a name → inode link.
- **FUSE:** Filesystem in Userspace.
- **Ratio:** original size ÷ stored size (e.g. 3 : 1). Here also given as
  stored size as a percentage of the original (e.g. 33 %).
- **Transaction:** one commit's metadata operations, written as one record
  and applied all or nothing.
- **Commit:** writing out dirty data and queued operations, then flushing.
- **Checkpoint:** a complete metadata snapshot in the log, which the
  superblock points to; mounting starts from it.
- **Seal:** the last record of a full segment, naming where the log continues.
- **Orphan:** a file deleted while still open; freed when the last user lets go.
- **Prefix consistency:** after a crash, the recovered state is that of some
  prefix of the log, a state that really existed.

---

## 18. References

- C. E. Shannon, "A Mathematical Theory of Communication," 1948.
- J. Ziv, A. Lempel, "A Universal Algorithm for Sequential Data Compression," 1977.
- D. A. Huffman, "A Method for the Construction of Minimum-Redundancy Codes," 1952.
- J. Duda, "Asymmetric numeral systems," 2009–2014.
- RFC 8878: Zstandard Compression and the `application/zstd` Media Type.
- LZ4 format: <https://github.com/lz4/lz4/tree/dev/doc>
- M. Rosenblum, J. Ousterhout, "The Design and Implementation of a Log-Structured File System," 1991.
- O. Rodeh, "B-trees, Shadowing, and Clones," ACM TOS, 2008.
- W. Xia et al., "FastCDC: a Fast and Efficient Content-Defined Chunking Approach for Data Deduplication," USENIX ATC 2016.
- N. Megiddo, D. Modha, "ARC: A Self-Tuning, Low Overhead Replacement Cache," FAST 2003.
- BLAKE3 specification: <https://github.com/BLAKE3-team/BLAKE3-specs>
- Linux FUSE documentation: <https://docs.kernel.org/filesystems/fuse.html>
- Btrfs compression: <https://btrfs.readthedocs.io/en/latest/Compression.html>
- EROFS: <https://docs.kernel.org/filesystems/erofs.html>
- Apache License 2.0: <https://www.apache.org/licenses/LICENSE-2.0>
