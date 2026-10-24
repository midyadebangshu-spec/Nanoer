# Nanoer on-disk format, version 1.0

This document is the authoritative definition of the format. A conforming
implementation (the C++ engine, a future kernel driver in C or Rust, a
recovery tool) must be writable from this document alone, without reading
any code. When the code and this document disagree, one of them is a bug.

Status: **1.0 draft**. It defines the store geometry, superblocks, segments,
records and data chunks. Metadata log entries and checkpoints (record types
2 and 3) are specified in milestone M2. Until Nanoer 1.0 is released, the
format may still change without a version bump.

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
| 139 | 5 | reserved | |
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

**Features.** No feature bits are defined in version 1.0.

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
| 2 | metadata log entry | M2 |
| 3 | checkpoint | M2 |

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
| 0 | `has_blake3` | the `blake3` field is present (deduplicated chunk) |
| 1–7 | — | reserved; a set unknown bit makes the record unreadable |

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

## 8. Changes

| Version | Change |
|---|---|
| 1.0 (draft) | initial: geometry, superblock, segments, records, data chunks |
