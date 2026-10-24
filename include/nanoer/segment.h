// Segments: fixed-size, append-only regions of the store that hold records.
#pragma once

#include <cstdint>
#include <optional>

#include "nanoer/bytes.h"
#include "nanoer/format.h"
#include "nanoer/record.h"

namespace nanoer {

class Store;

inline constexpr std::size_t kSegmentHeaderSize = kBlockSize;

struct SegmentHeader {
  Uuid uuid{};  // the filesystem's; rejects segments left by an earlier mkfs
  std::uint64_t seq = 0;  // position in the log; unique and increasing
  std::uint64_t index = 0;
  std::uint64_t created_ns = 0;
};

Bytes encode_segment_header(const SegmentHeader& header);

// Returns nullopt unless `block` is an intact header for segment `index` of
// the filesystem `uuid`.
std::optional<SegmentHeader> decode_segment_header(ByteView block, const Uuid& uuid,
                                                   std::uint64_t index);

std::optional<SegmentHeader> read_segment_header(Store& store, const Superblock& sb,
                                                 std::uint64_t index);

// Starts a new segment and appends records to it. Writes are buffered and
// reach the store in order; flush() makes them durable.
class SegmentWriter {
 public:
  SegmentWriter(Store& store, const Superblock& sb, std::uint64_t index, std::uint64_t seq);

  // Appends an encoded record and returns its absolute offset, or nullopt if
  // it does not fit in what is left of the segment.
  std::optional<std::uint64_t> append(ByteView encoded_record);

  // Hands everything buffered to the store, without waiting for it to be
  // durable.
  void write_buffered();

  // write_buffered(), then flushes the store: every appended record is durable.
  void flush();

  std::uint64_t index() const { return index_; }
  std::uint64_t seq() const { return seq_; }
  std::uint64_t used() const { return next_ - start_; }
  std::uint64_t remaining() const { return end_ - next_; }

 private:
  static constexpr std::size_t kWriteBufferSize = 1u << 20;

  Store& store_;
  std::uint64_t index_;
  std::uint64_t seq_;
  std::uint64_t start_;  // absolute offset of the segment
  std::uint64_t end_;
  std::uint64_t next_;   // absolute offset of the next record
  std::uint64_t buffer_start_;  // absolute offset of buffer_[0]
  Bytes buffer_;
};

// Walks the records of a segment in order, stopping where its log ends.
class SegmentScanner {
 public:
  SegmentScanner(Store& store, const Superblock& sb, std::uint64_t index);

  // False if the segment has no valid header: it is free (never written, or
  // left by an earlier filesystem).
  bool valid() const { return header_.has_value(); }
  const SegmentHeader& header() const { return *header_; }

  std::optional<Record> next();

  // Where the next record would go: just after the last intact record.
  std::uint64_t end_offset() const { return next_; }

 private:
  Store& store_;
  std::optional<SegmentHeader> header_;
  std::uint64_t next_;
  std::uint64_t end_;
  bool done_ = false;
};

}  // namespace nanoer
