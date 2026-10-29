# Nanoer on-disk format, version 1.0

This document is the authoritative definition of the format. A conforming
implementation (the C++ engine, a future kernel driver in C or Rust, a
recovery tool) must be writable from this document alone, without reading
any code. When the code and this document disagree, one of them is a bug.

Status: **1.0 draft**. It defines the store geometry, superblocks, segments,
records, data chunks, metadata transactions, checkpoints, the metadata tree,
segment seals, how the log is read back, and how segments are reused. Until Nanoer 1.0 is
released, the format may still change without a version bump.

## 1. Conventions

- All integers are **unsigned little-endian** unless marked signed.
- Offsets are in bytes. "Absolute" offsets count from the start of the store.
- **CRC-32C** is the Castagnoli CRC (reflected polynomial `0x82F63B78`,
  initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`). Check value: the CRC
  of the ASCII string `123456789` is `0xE3069283`.
- **XXH3-64** is `XXH3_64bits()` from xxHash 0.8 with seed 0.
- **BLAKE3** is the standard 256-bit BLAKE3 hash.
- Reserved fields and padding must be written as zero and ignored when read.
- Sizes written as `2^n` are stored as the exponent `n` ("shift").

## 2. Store layout

```
absolute offset
0          superblock slot 0      4096 bytes
4096       superblock slot 1      4096 bytes
8192       reserved               zero, up to data_start
data_start segment 0              2^segment_shift bytes
           segment 1
           ...
           segment segment_count-1
           unused tail            (store_size - data_start) mod 2^segment_shift
```

- Block size is fixed at 4096 bytes in version 1.
- `data_start` is 1 MiB (1048576) for filesystems created by this version.
  Readers must use the superblock value.
- Segment `i` begins at `data_start + i * 2^segment_shift`.

## 3. Superblock

Two copies (slots) of 4096 bytes each, at offsets 0 and 4096.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 8 | `magic` | ASCII `NANOERFS` |
| 8 | 2 | `version_major` | 1 |
| 10 | 2 | `version_minor` | 0 |
| 12 | 4 | `block_size` | 4096 |
| 16 | 16 | `uuid` | filesystem UUID (RFC 9562 version 4) |
| 32 | 8 | `generation` | incremented on every superblock write |
| 40 | 8 | `features_compat` | feature bits safe to ignore |
| 48 | 8 | `features_ro_compat` | unknown bits: mount read-only |
| 56 | 8 | `features_incompat` | unknown bits: refuse to mount |
| 64 | 8 | `store_size` | bytes of the store when formatted |
| 72 | 8 | `data_start` | absolute offset of segment 0 |
| 80 | 4 | `segment_shift` | segment size = 2^shift, 20..30 (1 MiB..1 GiB) |
| 84 | 4 | reserved | |
| 88 | 8 | `segment_count` | number of segments, at least 4 |
| 96 | 8 | `created_ns` | creation time, nanoseconds since the Unix epoch (UTC) |
| 104 | 8 | `updated_ns` | time of this superblock write |
| 112 | 8 | `next_segment_seq` | sequence number the next new segment will get, ≥ 1 |
| 120 | 8 | `checkpoint_offset` | absolute offset of the latest checkpoint record; 0 = none |
| 128 | 8 | `checkpoint_seq` | sequence number of the segment holding it |
| 136 | 1 | `default_codec` | codec id for new data (§7) |
| 137 | 1 | `default_level` | **signed** compression level for new data |
| 138 | 1 | `chunk_shift` | chunk size = 2^shift, 12..24 (4 KiB..16 MiB) |
| 139 | 1 | reserved | |
| 140 | 4 | `state` | state flags, below |
| 144 | 64 | `label` | UTF-8, NUL-padded, at most 63 bytes |
| 208 | 3884 | reserved | |
| 4092 | 4 | `crc` | CRC-32C of bytes 0..4091 |

**Validity.** A slot is *valid* when the magic matches, the CRC matches,
`version_major` is 1, `features_incompat` has no unknown bits, and every
field is consistent:

- `block_size` is 4096; `segment_shift` is 20..30; `chunk_shift` is 12..24
  and smaller than `segment_shift`;
- `default_codec` is a known codec, and `default_level` is within its range;
- `data_start` is a multiple of 4096, at least 8192, and at most `store_size`;
- `segment_count` is at least 4, and all segments fit within `store_size`;
- `next_segment_seq` is at least 1;
- `checkpoint_offset` is 0 or lies inside the segment area;
- the label contains a NUL within its 64 bytes.

A slot with the right magic and CRC whose `version_major` is not 1, or that
has unknown `features_incompat` bits, is *unsupported*.

**Choosing a slot (mount).**

1. Decode both slots. Ignore invalid ones.
2. Of the rest, take the one with the highest `generation`.
3. If that one is *unsupported*, refuse to mount. Falling back to the other
   slot would silently drop newer changes.
4. If no slot remains, the filesystem is corrupt.
5. If `store_size` exceeds the store's actual size, the store has been
   truncated: refuse to mount.
6. Unknown `features_ro_compat` bits: mount read-only.

**Writing.** Increment `generation`, then write the superblock to slot
`generation mod 2` and flush. The other slot keeps the previous valid
superblock, so a torn write can never leave both slots unusable. Data the
new superblock refers to must be durable before the superblock is written.

`mkfs` writes generation 1 (slot 1) and then generation 2 (slot 0), so both
slots are valid from the start.

**State flags.** Bit 0 (`errors`): damage has been seen, in metadata or
data; set by the first checkpoint after the damage is noticed, cleared
only by `fsck` once it has found or repaired everything. A mount reports
it; a boot-time `fsck -a` then knows the filesystem needs a full check.
Other bits are reserved and written as zero. An unknown bit set is not an
error (old readers ignore the field), but writers preserve it.

**Features.**

| Kind | Bit | Name | Meaning |
|---|---:|---|---|
| compat | 0 | `no_dedup` | a preference: writers do not deduplicate new data (`mkfs.nanoer --no-dedup`) |
| incompat | 0 | `shared_records` | a data chunk record may have several references (dedup) or hold several small files (a pack); the log may contain SetChunkRef (§8) and checkpoints use version 2 (§9) |
| incompat | 1 | `cold_data` | extended references may carry the `cold` flag (§8); only with `shared_records` (ignored without it) |
| incompat | 2 | `metadata_tree` | the metadata is a B+tree of nodes in the log (§9.2): checkpoints use version 3 and the log holds tree node records (type 5); extended references may carry `owner` (§8); only with `shared_records` |

`mkfs` sets `shared_records`, `cold_data` and `metadata_tree` on every new
filesystem. Without them a filesystem is exactly as earlier drafts of 1.0
wrote it, and stays so. A filesystem rescued from a version 2 checkpoint (no
valid superblock left) gets the first two; from a version 3 checkpoint, all
three. A filesystem without `metadata_tree` (made before M6) is read by
loading its whole checkpoint into memory, as before; writers mount it
read-only until it is upgraded.

**Upgrading** (`nanoer upgrade`) a filesystem without `metadata_tree`:
replay it (§10.2), write its metadata as a tree (§9.2) into free segments
after the end of its log (sealed there, §10.3), then a version 3
checkpoint, flush, and write a superblock with all three incompat features
set, pointing at that checkpoint. Records a dedup share left with a single
user whose header names another chunk (§8, *back-references*) get a refs
item. Until that superblock is written, nothing the old filesystem uses
has changed; a crash before it leaves the old filesystem, after it the
upgraded one.

## 4. Segments

A segment is a fixed-size, append-only region. It holds a 4096-byte header
followed by records.

### 4.1 Segment header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 8 | `magic` | ASCII `NNRSEG01` |
| 8 | 16 | `uuid` | the filesystem's UUID |
| 24 | 8 | `seq` | sequence number, ≥ 1 |
| 32 | 8 | `index` | this segment's index |
| 40 | 8 | `created_ns` | when the segment was started |
| 48 | 4044 | reserved | |
| 4092 | 4 | `crc` | CRC-32C of bytes 0..4091 |

A segment is **in use** only if its header is intact (magic and CRC), and
`uuid` and `index` match. Anything else (zeros, an old filesystem's
segment, a damaged header) is a **free** segment. This is why `mkfs` does
not need to clear segments.

**Sequence numbers** order the log. Each time a segment is started, or
restarted after garbage collection frees it, it gets the next number from
`next_segment_seq`, which then increases. No two segments in use share a
sequence number.

### 4.2 Records inside a segment

Records start right after the header (segment offset 4096). Each record
starts at an offset that is a multiple of 8, directly after the previous
record's padded end. The log of a segment ends at the first position where
no intact record with the segment's `seq` begins (§5.2). Bytes after that
position are undefined.

## 5. Records

### 5.1 Common header (32 bytes)

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x4345524E` (bytes `N R E C`) |
| 4 | 1 | `type` | §5.3 |
| 5 | 1 | `flags` | type-specific |
| 6 | 2 | `header_len` | common + type-specific header bytes; multiple of 8, 32..256 |
| 8 | 4 | `payload_len` | payload bytes |
| 12 | 4 | `payload_crc` | CRC-32C of the payload |
| 16 | 8 | `seq` | the `seq` of the segment the record was written into |
| 24 | 4 | reserved | |
| 28 | 4 | `header_crc` | CRC-32C of the `header_len` header bytes, this field counted as zero |

The type-specific header follows at offset 32, then the payload at offset
`header_len`. The record is padded with zeros to a multiple of 8 bytes:
`stored_size = align8(header_len + payload_len)`.

### 5.2 Validity

A record at a given position is *intact* when the magic matches, `seq`
equals the segment's `seq`, `header_len` is a multiple of 8 in 32..256, the
whole record fits inside the segment, `header_crc` matches, and
`payload_crc` matches.

When scanning, the first non-intact position ends the segment's log. The
`seq` check rejects records left over from an earlier use of the segment;
the CRCs reject torn writes. When metadata points at a record that is not
intact, the filesystem is corrupt: never return its data.

### 5.3 Record types

| Type | Name | Defined in |
|---:|---|---|
| 1 | data chunk | §6 |
| 2 | metadata transaction | §8 |
| 3 | checkpoint part | §9 |
| 4 | segment seal | §10 |
| 5 | tree node | §9.2; only with `metadata_tree` |

An unknown record type in the log makes the filesystem unreadable.

## 6. Data chunk record (type 1)

Holds one chunk of file data, compressed or stored raw.

Type-specific header (32 bytes, or 64 with `has_blake3`), starting at
record offset 32:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 32 | 1 | `codec` | §7 |
| 33 | 3 | reserved | |
| 36 | 4 | `uncompressed_size` | at most 2^24 (16 MiB) |
| 40 | 8 | `xxh3` | XXH3-64 of the uncompressed data |
| 48 | 8 | `inode` | the file the chunk was first written for |
| 56 | 8 | `file_offset` | its byte position in that file |
| 64 | 32 | `blake3` | BLAKE3 of the uncompressed data; only with `has_blake3` |

Flags:

| Bit | Name | Meaning |
|---:|---|---|
| 0 | `has_blake3` | the `blake3` field is present: the chunk has an identity and may be shared |
| 1 | `pack` | the data is several small files' contents, back to back (§8, *pieces*); `inode` and `file_offset` are 0, and `has_blake3` is not set |
| 2–7 | — | reserved; a set unknown bit makes the record unreadable |

`inode` and `file_offset` name the chunk's one owner only when neither
flag is set: data with an identity may be shared, and its first owner may
be gone.

The payload is the compressed data (or the raw data for codec 0, where
`payload_len` must equal `uncompressed_size`).

**Reading** a chunk: check the record is intact (§5.2), decompress the
payload into exactly `uncompressed_size` bytes, and compare its XXH3-64 with
`xxh3`. Any failure means the chunk is corrupt.

`inode` and `file_offset` record the chunk's original owner. They let `fsck`
and garbage collection identify chunks by scanning segments alone. The
filesystem metadata (M2), not these fields, decides which chunks are live.

## 7. Codecs

| Id | Name | Payload format |
|---:|---|---|
| 0 | none | the raw bytes |
| 1 | lz4 | one LZ4 **block** (not the LZ4 frame format) |
| 2 | zstd | one Zstandard frame (RFC 8878) |
| 3 | lzma | one **.xz** stream with check type None, holding one LZMA2 filter |

Levels by codec: lz4 0..12 (0–2 use the fast compressor, 3–12 use LZ4HC),
zstd −7..22, lzma 0..9 (xz presets). Levels affect only compression; any
conforming decoder reads any level.

For lzma, writers shrink the LZMA2 dictionary to the smallest power of two
covering the chunk (at least 4 KiB). Readers should refuse streams that need
more than 128 MiB of decoder memory.

The Linux kernel already contains decoders for all three (`lib/lz4`,
`lib/zstd`, `lib/xz`); the .xz container was chosen for lzma partly because
the kernel's xz decoder reads it directly.

## 8. Metadata transactions (type 2)

The filesystem's metadata is changed only by **operations**. A transaction
record holds the operations of one commit, applied in order, all or nothing
(the record is either intact or not part of the log at all).

Type-specific header (16 bytes):

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 32 | 8 | `txid` | transaction id; each is exactly one more than the previous |
| 40 | 4 | `op_count` | number of operations in the payload |
| 44 | 4 | `durable_back` | `txid − durable_back` is the newest transaction that was durable when this one was written; 0 = unknown |

`durable_back` is what lets a reader tell damage from a crash (§10.2). A
writer sets it from the transactions its last completed flush made
durable; it writes 0 when that distance does not fit in 32 bits.

The payload is the operations, back to back, in this encoding:

- `varint`: unsigned LEB128, at most 10 bytes, at most 2^64−1.
- `svarint`: zigzag (`(n << 1) ^ (n >> 63)`), then `varint`.
- `bytes(max)`: `varint` length (at most `max`), then that many bytes.

Each operation is a one-byte code followed by its fields:

| Code | Operation | Fields | Effect |
|---:|---|---|---|
| 1 | PutInode | *attributes* (below) | create the inode, or replace its attributes; its chunks, entries and xattrs are kept |
| 2 | DeleteInode | `ino` | remove the inode with its chunks and xattrs (never the root) |
| 3 | SetEntry | `dir`, `name` bytes(255), `ino` | add or replace directory entry `name` |
| 4 | RemoveEntry | `dir`, `name` bytes(255) | remove an existing entry |
| 5 | SetChunk | `ino`, `index`, `offset`, `stored_size`, `size` | chunk `index` of a regular file is now the data chunk record at absolute `offset` |
| 6 | RemoveChunk | `ino`, `index` | chunk becomes a hole (no-op if it already is one) |
| 7 | TruncateChunks | `ino`, `first_index` | remove every chunk with index ≥ `first_index` |
| 8 | SetXattr | `ino`, `name` bytes(255), `value` bytes(65536) | add or replace |
| 9 | RemoveXattr | `ino`, `name` bytes(255) | remove an existing xattr |
| 10 | SetChunkRef | `ino`, `index`, *extended reference* (below) | like SetChunk; only with `shared_records` |

All fields are `varint` unless marked. *Attributes*, in order:
`id`, `mode` (st_mode, type and permission bits), `uid`, `gid`, `nlink`,
`size`, `atime`, `mtime`, `ctime`, `btime` (these four `svarint`, nanoseconds
since the Unix epoch), `rdev`, `parent` (directories: the containing
directory; root: itself; others: 0), `symlink` (bytes(4095): the target of a
symlink, else empty).

An *extended reference* is `flags` (varint), `offset`, `stored_size`,
`size`, then `piece` (varint) if flag 0 is set, then `key` (8 bytes) if
flag 1 is set:

| Flag | Name | Meaning |
|---:|---|---|
| 0 | `packed` | the record is a pack; this chunk is the `size` bytes at `piece` in its uncompressed data |
| 1 | `hash` | `key`: the first 8 bytes of the chunk data's BLAKE3, little-endian, never 0 (the dedup key) |
| 2 | `share` | the record already has a reference; this is one more (dedup). Not with `packed` |
| 3 | `cold` | the chunk's data has been recompressed in its cold form (§8, *cold data*); only with `cold_data`, else the log is damaged |
| 4 | `owner` | then `ino`, `index` (varint): the chunk the record's header names (see below); only with `share`, not with `packed`, and only with `metadata_tree` |
| 5+ | — | reserved; a set unknown flag means the log is damaged |

The fields come in flag order: `piece`, `key`, then `ino` and `index`.

**Space and references** (with `shared_records`). A record's space
(`stored_size`) counts once, however many chunks refer to it. A reader
keeps a reference count for every pack and every other record with more
than one reference:

- a SetChunk, or a SetChunkRef without `share` or `packed`, refers to a
  record nothing else refers to yet: its space is claimed;
- a SetChunkRef with `share` adds a reference to a record that has one;
  a writer adds one only to a record whose `blake3` equals the BLAKE3 of
  the chunk's data (dedup), and never to a record without `has_blake3`;
- the first piece of a pack claims the pack's space, later pieces add a
  reference; pieces of one pack never overlap, and a piece may be
  referred to by several chunks (dedup of small files);
- removing or replacing a chunk drops one reference; the last one
  releases the space.

Without `shared_records`, every chunk claims space of its own, and a
record referred to twice is damage.

**Back-references** (with `metadata_tree`). The tree (§9.2) lists the
users of every pack and of every record that has been shared, in a *refs*
item; any other record has exactly one user, the chunk its header names
(`inode`, `file_offset / chunk_size`). Garbage collection, which finds
records by scanning segments, uses the one or the other to find who
refers to a record. So:

- the first SetChunkRef with `share` to a record without a refs item
  carries `owner`, the chunk that refers to it now (whatever its header
  says); the refs item is created with that user and the new one;
- once created, a refs item stays as long as the record has a reference,
  even one (a *sticky* item): the remaining user need not be the one the
  header names;
- a copy of a record without a refs item (garbage collection) names its
  one user in its header.

**Cold data** (with `cold_data`). A writer may rewrite data that has stopped
changing in a denser form, and marks every reference to the new record
`cold`, so that it is not examined again. The flag changes nothing in how
the reference is read or counted. A cold record is either

- a single chunk rewritten with another codec or level, with the same
  `inode`, `file_offset` and `blake3` header fields as before; or
- a pack (`pack` flag) of small files recompressed, its live pieces in
  the same order.

Any pack is valid for a reader, whatever its pieces; an early M5 build
also wrote packs of one file's consecutive chunks (*cold units*).

A reference may also be marked `cold` in place (a SetChunkRef naming the
record it already refers to, which releases and claims the same space)
when recompressing would not have made it smaller. Readers need not tell
these apart.

**Policies.** The extended attribute `user.nanoer.policy` on a file or
directory, with the value `none`, `fast`, `balanced` or `max` (exactly
these bytes), says how its data is to be stored (docs/M5_PLAN.md §4). A
new file or directory receives a copy of its parent's. No other name
beginning `user.nanoer.` is stored. The attribute changes nothing for
readers.

Rules (a violation means the log is damaged):

- Inode 0 does not exist; inode 1 is the root directory.
- Entry names are 1–255 bytes, contain neither `/` nor NUL, and are not `.`
  or `..`. Entries live only in directories and point at existing inodes.
- Chunks belong only to regular files. Chunk *i* covers file bytes
  `[i × chunk_size, (i+1) × chunk_size)`; a missing chunk reads as zeros, as
  do the bytes between a chunk's `size` and the end of its range.
- A chunk's `offset` must lie in a segment that is in use (§10).
- After an inode is created, the allocation counter is at least its id + 1;
  inode numbers are never reused while that inode exists.

## 9. Checkpoints (type 3) and the metadata tree (type 5)

A checkpoint is the state of the metadata as of one transaction, so
mounting does not have to replay the whole history. Without
`metadata_tree` it is a complete snapshot (versions 1 and 2); with it, the
metadata lives in a B+tree whose nodes are records in the log, and the
checkpoint names the tree's root (version 3, §9.2).

### 9.1 Checkpoint records

The checkpoint's stream can be large (version 1 and 2), so it is split
into **parts**, written one after another in log order (possibly across
segments). The superblock's `checkpoint_offset` points at part 0.

Type-specific header (32 bytes), the same values in every part except
`part` and the final flag:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 32 | 8 | `txid` | the last transaction the snapshot includes |
| 40 | 4 | `part` | 0, 1, 2, … |
| 44 | 4 | `flags` | bit 0: this is the final part |
| 48 | 8 | `total` | length of the whole snapshot stream |
| 56 | 8 | `xxh3` | XXH3-64 of the whole snapshot stream |

The payloads of the parts, concatenated, form the **snapshot stream**:

```
"NNRCKPT1" or "NNRCKPT2"           8 bytes; version 2 only with shared_records
last_txid, next_inode              varint
segment_count                      varint
  index, seq, live_bytes           varint, per segment in use
inode_count                        varint
  attributes                       as in §8, per inode, in increasing id order
  chunk_count                      varint
    index, offset, stored_size, size   varint, per chunk (version 1)
    index, extended reference      per chunk (version 2; §8)
  entry_count                      varint
    name bytes(255), ino           per entry
  xattr_count                      varint
    name bytes(255), value bytes(65536)   per xattr
```

`live_bytes` of a segment is the sum of `stored_size` of the records stored
in it that something refers to, each counted once (§8); a reader must check
that the recorded value matches the chunks. In version 2, `share` marks
every reference to a record that has more than one, so a reader counts
references only for those and for packs; a record marked `share` with a
single reference, or references to one record that disagree on
`stored_size` or `packed`, mean the checkpoint is damaged. A
reader must also check that the root exists and is a directory, that every
entry resolves, and that no id reaches `next_inode`.

### 9.2 The metadata tree (with `metadata_tree`)

**Stream, version 3.**

```
"NNRCKPT3"                         8 bytes
last_txid, next_inode              varint
root_offset, root_size             varint: the root node's record (0, 0: an empty tree)
node_bytes                         varint: stored_size of every node record in the tree
files, directories, other_inodes   varint: inodes by type
logical_bytes                      varint: sum of the regular files' sizes
segment_count                      varint
  index, seq, live_bytes           varint, per segment in use
```

`live_bytes` of a segment counts the data records referred to (§8) and the
tree node records the tree holds, each once. A writer writes the tree's
changed nodes before the checkpoint that names them; all of them are in
the log before the checkpoint's first part.

**Node records (type 5).** Type-specific header (48 bytes), from record
offset 32:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 32 | 1 | `codec` | §7 |
| 33 | 1 | `level` | 0: a leaf |
| 34 | 2 | reserved | |
| 36 | 4 | `count` | items (leaf) or children (inner node) |
| 40 | 4 | `uncompressed_size` | of the node encoding, at most 64 MiB |
| 44 | 4 | reserved | |
| 48 | 8 | `xxh3` | XXH3-64 of the node encoding |
| 56 | 17 | `first` | the node's first key (below) |
| 73 | 7 | reserved | |

The payload is the node encoding, compressed with `codec` (the engine uses
zstd level 1; nodes are 16 KiB uncompressed, at most). `level`, `count`
and `first` repeat what the node holds, so garbage collection can find a
node in the tree (from its first key, down to its level) by scanning
segments alone. `fsck` checks them against the node.

**Node encoding.** `level` (u8), `count` (varint), then `count` keys in
strictly increasing order, then for a leaf `count` values (`bytes`, at
most 16 MiB each), for an inner node `count` children (`offset`,
`stored_size` varint: a node record; neither 0). A key is 17 bytes:
`object` (u64), `kind` (u8), `offset` (u64), little-endian, ordered by
`object`, then `kind`, then `offset`. An inner node's keys are its
children's first keys; each child's level is one less than its parent's;
leaves are at level 0. Copy on write: a changed node is written as a new
record and its parent changed to point at it, up to a new root; the old
records become dead space.

**Items.** Every key in a leaf is an item:

| Kind | Object | Offset | Value |
|---:|---|---|---|
| 1 inode | inode | 0 | attributes (§8) |
| 2 counts | inode | 0 | `entries`, `data_bytes` (varint): a directory's entries; a file's chunks' `size` summed. Absent when both are 0 |
| 3 xattr | inode | XXH3-64 of the name | `n` (varint, at least 1), then `n` × (name bytes(255), value bytes(65536)): the xattrs whose names have that hash |
| 4 entry | directory | XXH3-64 of the name | `n` (varint, at least 1), then `n` × (name bytes(255), `ino` varint): the entries whose names have that hash |
| 5 chunk | inode | chunk index | an extended reference (§8) without `share` or `owner` |
| 6 orphan | 0 | inode | empty: an inode other than the root with `nlink` 0, freed once nothing has it open |
| 7 refs | 2^63 + record offset | 0 | `count`, `n` (varint, `n` = `count`), then `n` × (`ino`, `index` varint): the record's users (§8, *back-references*) |

Every item of an inode follows its inode item (they share `object`, and
kind 1 sorts first). `fsck` checks that names hash to their key, that no
name appears twice, that a file's chunk items agree with the refs items
(every pack and every refs item has exactly its users), and that the
counters and `live_bytes` match the items. An orphan item exists exactly
for each inode with `nlink` 0 (not the root).

Applying an operation (§8) changes the items: PutInode writes the inode
item (and the orphan item); DeleteInode removes every item of the inode;
SetEntry and RemoveEntry change the entry item of the name's hash and the
directory's `entries` count; SetChunk, SetChunkRef, RemoveChunk and
TruncateChunks the chunk items, `data_bytes`, refs items and `live_bytes`;
SetXattr and RemoveXattr the xattr item of the name's hash.

## 10. Segment seals (type 4), reading the log, and reusing segments

### 10.1 Seals

When a segment is finished, a seal record is its last record:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 32 | 8 | `next_index` | the segment where the log continues |
| 40 | 8 | `next_seq` | that segment's sequence number |

A writer keeps 48 bytes (one seal) free at the end of every segment, so a
seal always fits.

### 10.2 Reading the log (mount)

1. Load the superblock (§3). Read every segment header and note the highest
   sequence number found: new segments must get higher numbers, even than
   segments that were started but never linked into the log.
2. Start at `checkpoint_offset`, in the segment with `checkpoint_seq`.
3. Read checkpoint parts 0, 1, … until the final one. A missing part, a
   mismatch between parts, or a wrong `total` or `xxh3` means corruption.
4. Decode the snapshot. Then read on, record by record:
   - data chunk: skip (transactions refer to it);
   - transaction: its `txid` must be one more than the last; apply its
     operations in order;
   - checkpoint part: skip (a later checkpoint the superblock never pointed
     to; the transactions around it give the same state);
   - tree node: skip (a checkpoint after it names it, if any);
   - seal: if segment `next_index` has an intact header with sequence number
     `next_seq`, continue at its first record; otherwise the log ends at the
     seal itself.
5. The log ends at the first position where no intact record (§5.2) begins.
6. **Damage or crash?** A crash can only cut the log short where writes
   were not yet flushed, and nothing written after an unflushed write can
   claim that a later transaction was flushed. So scan on past the end: in
   the rest of that segment, at every 8-byte boundary, for intact records
   of the current sequence number, following any intact seals found there
   into later segments (and into the segment a seal at the end points to,
   even if its header is damaged). Each transaction found *claims*
   `txid − durable_back` (when `durable_back` ≠ 0), each checkpoint part
   claims its `txid`. If any claim exceeds the last transaction applied,
   the log is **damaged**, not torn: the recovered state is missing
   committed transactions. A reader must not then write to the
   filesystem: it mounts read-only and asks for `fsck`.
7. A transaction that is intact but contradicts the metadata (an
   operation that breaks a rule of §8) is damage too; replay stops before
   it, and step 6 applies with the log ending there.
8. Every segment the reading entered is in use. If the snapshot listed that
   segment with another sequence number, it must have had no live data
   (it was freed and reused since); otherwise the filesystem is corrupt.

The recovered state is always a **prefix** of what was written: reading
never skips a damaged record to continue after it, and a new segment is
only entered through an intact seal. Writers append each data chunk before
the transaction that refers to it, so a transaction in the prefix never
refers to a missing chunk.

**Limit.** Damage to the last transaction alone, with nothing intact after
it, looks exactly like a crash during that commit: no later record claims
it. It is lost without a report. A writer narrows this window by
committing again soon (every later transaction claims the earlier ones).

**Checkpoint fallback.** If the checkpoint the newest superblock points to
cannot be read (step 3), or (with `metadata_tree`) its tree's root node
cannot, or a node read while replaying cannot, a reader uses the other slot, if it is valid,
older and of the same filesystem, and reads its checkpoint and the log
after it instead. The transactions after the older checkpoint lead to the
same state as the damaged one, provided the segments they are in were not
reused (§10.4). If neither checkpoint can be read, `fsck --repair`
searches every segment for checkpoint part 0 records, newest `txid` first,
and uses the first one that reads and replays cleanly.

With `metadata_tree`, consecutive checkpoints share every node that did
not change, so a damaged node other than the root is usually in both.
`fsck --repair` then *salvages* the newest checkpoint instead, if its root
can be read: it keeps every item in the nodes it can read, gives up the
nodes it cannot and the log after the checkpoint, and repairs the rest as
any other damage (entries to lost inodes removed, inodes without a
directory moved to `lost+found`); then it writes the tree again.

### 10.3 Continuing the log after mount

A writer never appends after the end found in §10.2, because that end may
be a torn write. It writes a seal **at** the end position, pointing at a
newly allocated segment, and continues there.

### 10.4 Reusing segments

A segment is **in use** while the metadata lists it. A segment with no live
data, older than the newest checkpoint, is not needed by that checkpoint,
but it may still be needed by the previous one, which the other superblock
slot points to, should the newest superblock write be torn. Therefore:

- When checkpoint *K* is durable and the superblock points to it, segments
  with no live data and a sequence number below *K*'s are **released**.
  They become free for reuse only once checkpoint *K+1* is durable.
- At mount from checkpoint *K*, such segments may be reused at once: the
  superblock pointing at *K* was read from disk, so it is durable, and it
  stays intact until two more superblocks are written. This gives up the
  checkpoint fallback of §10.2 for *K* (the older checkpoint's log may be
  overwritten), so a writer does it only when it is short of free
  segments.
- Each time a segment is (re)started it gets a new, higher sequence number,
  so records left from its earlier use can never be mistaken for current
  ones (§5.2).
- A free segment may be **discarded**: a writer tells the layer below that
  its contents are no longer needed (a hole punched in an image file,
  TRIM on a device), so it may read back as zeros, in whole or in part.
  Only segments a writer could write over at that moment are discarded,
  so this needs no format change, and a reader treats a segment whose
  header is zeros as free, like any other without a valid header.

## 11. A new filesystem

`mkfs` writes segment 0 (sequence number 1) holding a checkpoint of the
empty filesystem: the root directory (inode 1, a directory with mode 0755,
`nlink` 2, `parent` 1), `next_inode` 2, `last_txid` 0, and segment 0 in use.
With `metadata_tree`, the root's inode item is the one leaf of the tree,
written just before the checkpoint.
Both superblocks point at it, with `next_segment_seq` 2. Every filesystem
therefore always has a checkpoint.

## 12. Changes

| Version | Change |
|---|---|
| 1.0 (draft) | initial: geometry, superblock, segments, records, data chunks |
| 1.0 (draft, M2) | metadata transactions, checkpoints, segment seals, log replay, segment reuse; mkfs writes an initial checkpoint |
| 1.0 (draft, M3) | transaction `durable_back` (was reserved, so older images read as "unknown"); superblock `state` flags (was reserved); damage detection, contradictory transactions, checkpoint fallback and rescue (§10.2) |
| 1.0 (draft, M4) | `shared_records` feature: SetChunkRef, shared records and packs, reference counting, checkpoint version 2; data chunk `pack` flag; `no_dedup` compat preference; dedup shares only records whose `blake3` matches |
| 1.0 (draft, M5) | `cold_data` feature: reference flag `cold`, cold records; the `user.nanoer.policy` attribute |
| 1.0 (draft, M6) | `metadata_tree` feature: the metadata as a B+tree of node records (type 5), checkpoint version 3, back-references (refs items) and the reference flag `owner` |
