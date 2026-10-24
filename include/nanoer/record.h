// Records: the self-describing units appended to segments.
#pragma once

#include <cstdint>
#include <optional>

#include "nanoer/bytes.h"
#include "nanoer/checksum.h"
#include "nanoer/codec.h"

namespace nanoer {

class Store;

enum class RecordType : std::uint8_t {
  data_chunk = 1,
  metadata_log = 2,  // one transaction of metadata operations
  checkpoint = 3,    // one part of a metadata snapshot
  segment_seal = 4,  // the segment is finished; the log continues elsewhere
};

inline constexpr std::uint32_t kRecordMagic = 0x4345524e;  // "NREC" on disk
inline constexpr std::size_t kRecordHeaderSize = 32;
inline constexpr std::size_t kRecordAlignment = 8;
inline constexpr std::size_t kMaxRecordHeaderSize = 256;
inline constexpr std::size_t kDataChunkHeaderSize = kRecordHeaderSize + 32;
inline constexpr std::size_t kDataChunkHeaderSizeWithId = kDataChunkHeaderSize + 32;

namespace data_chunk_flags {
inline constexpr std::uint8_t has_blake3 = 0x01;
inline constexpr std::uint8_t known = has_blake3;
}  // namespace data_chunk_flags

struct RecordHeader {
  RecordType type{};
  std::uint8_t flags = 0;
  std::uint16_t header_len = 0;  // common header + type-specific header
  std::uint32_t payload_len = 0;
  std::uint32_t payload_crc = 0;
  std::uint64_t seq = 0;  // sequence number of the segment that holds it
};

struct Record {
  std::uint64_t offset = 0;  // absolute position in the store
  RecordHeader header;
  Bytes extra;    // type-specific header (header_len - kRecordHeaderSize bytes)
  Bytes payload;

  // Bytes the record occupies, including padding to kRecordAlignment.
  std::uint64_t stored_size() const {
    return align_up(std::uint64_t{header.header_len} + header.payload_len, kRecordAlignment);
  }
};

// Encodes a complete record, padded to kRecordAlignment.
Bytes encode_record(RecordType type, std::uint8_t flags, std::uint64_t seq, ByteView extra,
                    ByteView payload);

// Reads the record at `offset`, which must end at or before `limit`.
// Returns nullopt if no intact record with sequence number `seq` is there:
// that is where a segment's log ends (or where a torn write was cut off).
std::optional<Record> try_read_record(Store& store, std::uint64_t offset, std::uint64_t limit,
                                      std::uint64_t seq);

// Like try_read_record, but checks only the header (its CRC covers the
// type-specific fields); the payload is neither read nor verified.
std::optional<Record> try_read_record_header(Store& store, std::uint64_t offset,
                                             std::uint64_t limit, std::uint64_t seq);

// Like try_read_record, for a location the metadata points to: a missing or
// damaged record there is corruption, so it throws Error(corrupt).
Record read_record(Store& store, std::uint64_t offset, std::uint64_t limit, std::uint64_t seq);

struct DataChunkInfo {
  CodecId codec{};
  std::uint32_t uncompressed_size = 0;
  std::uint64_t xxh3 = 0;
  // The file and position the chunk was first written for. Lets fsck and GC
  // work out what a chunk belongs to by scanning, without the metadata.
  std::uint64_t inode = 0;
  std::uint64_t file_offset = 0;
  std::optional<Blake3Hash> blake3;  // content identity, when deduplicated
};

Bytes encode_data_chunk_record(std::uint64_t seq, const EncodedChunk& chunk, std::uint64_t inode,
                               std::uint64_t file_offset,
                               const std::optional<Blake3Hash>& blake3 = std::nullopt);

// Parses a data chunk record's type-specific header. Throws Error(corrupt).
DataChunkInfo parse_data_chunk(const Record& record);

// Decompresses and verifies a data chunk record. Throws Error(corrupt).
Bytes decode_data_chunk(const Record& record);

}  // namespace nanoer
