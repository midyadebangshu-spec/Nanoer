#include "nanoer/segment.h"

#include <cstring>

#include "nanoer/checksum.h"
#include "nanoer/error.h"
#include "nanoer/io.h"

namespace nanoer {
namespace {

constexpr char kMagic[8] = {'N', 'N', 'R', 'S', 'E', 'G', '0', '1'};

namespace off {
constexpr std::size_t magic = 0;
constexpr std::size_t uuid = 8;
constexpr std::size_t seq = 24;
constexpr std::size_t index = 32;
constexpr std::size_t created_ns = 40;
constexpr std::size_t crc = kSegmentHeaderSize - 4;
}  // namespace off

void check_index(const Superblock& sb, std::uint64_t index) {
  if (index >= sb.segment_count) {
    throw Error(Errc::invalid_argument, "segment " + std::to_string(index) + " does not exist");
  }
}

}  // namespace

Bytes encode_segment_header(const SegmentHeader& h) {
  Bytes b(kSegmentHeaderSize, 0);
  std::memcpy(b.data() + off::magic, kMagic, sizeof kMagic);
  std::memcpy(b.data() + off::uuid, h.uuid.data(), h.uuid.size());
  store_le(b.data() + off::seq, h.seq);
  store_le(b.data() + off::index, h.index);
  store_le(b.data() + off::created_ns, h.created_ns);
  store_le(b.data() + off::crc, crc32c(ByteView(b.data(), off::crc)));
  return b;
}

std::optional<SegmentHeader> decode_segment_header(ByteView b, const Uuid& uuid,
                                                   std::uint64_t index) {
  if (b.size() != kSegmentHeaderSize) return std::nullopt;
  if (std::memcmp(b.data() + off::magic, kMagic, sizeof kMagic) != 0) return std::nullopt;
  if (load_le<std::uint32_t>(b.data() + off::crc) != crc32c(b.first(off::crc))) return std::nullopt;
  SegmentHeader h;
  std::memcpy(h.uuid.data(), b.data() + off::uuid, h.uuid.size());
  h.seq = load_le<std::uint64_t>(b.data() + off::seq);
  h.index = load_le<std::uint64_t>(b.data() + off::index);
  h.created_ns = load_le<std::uint64_t>(b.data() + off::created_ns);
  if (h.uuid != uuid || h.index != index || h.seq == 0) return std::nullopt;
  return h;
}

std::optional<SegmentHeader> read_segment_header(Store& store, const Superblock& sb,
                                                 std::uint64_t index) {
  check_index(sb, index);
  return decode_segment_header(store.read(sb.segment_offset(index), kSegmentHeaderSize), sb.uuid,
                               index);
}

// ---- SegmentWriter ---------------------------------------------------------

SegmentWriter::SegmentWriter(Store& store, const Superblock& sb, std::uint64_t index,
                             std::uint64_t seq)
    : store_(store), index_(index), seq_(seq) {
  check_index(sb, index);
  if (seq == 0) throw Error(Errc::invalid_argument, "segment sequence numbers start at 1");
  start_ = sb.segment_offset(index);
  end_ = start_ + sb.segment_size();
  buffer_start_ = start_;
  buffer_ = encode_segment_header({sb.uuid, seq, index, now_ns()});
  buffer_.reserve(kWriteBufferSize + kSegmentHeaderSize);
  next_ = start_ + kSegmentHeaderSize;
}

std::optional<std::uint64_t> SegmentWriter::append(ByteView record) {
  if (record.size() < kRecordHeaderSize || record.size() % kRecordAlignment != 0) {
    throw Error(Errc::invalid_argument, "not an encoded record");
  }
  if (record.size() > remaining()) return std::nullopt;
  std::uint64_t at = next_;
  buffer_.insert(buffer_.end(), record.begin(), record.end());
  next_ += record.size();
  if (buffer_.size() >= kWriteBufferSize) write_buffered();
  return at;
}

void SegmentWriter::write_buffered() {
  if (buffer_.empty()) return;
  store_.write(buffer_start_, buffer_);
  buffer_start_ += buffer_.size();
  buffer_.clear();
}

void SegmentWriter::flush() {
  write_buffered();
  store_.flush();
}

// ---- SegmentScanner --------------------------------------------------------

SegmentScanner::SegmentScanner(Store& store, const Superblock& sb, std::uint64_t index)
    : store_(store), header_(read_segment_header(store, sb, index)) {
  next_ = sb.segment_offset(index) + kSegmentHeaderSize;
  end_ = sb.segment_offset(index) + sb.segment_size();
}

std::optional<Record> SegmentScanner::next() {
  if (done_ || !header_) return std::nullopt;
  auto record = try_read_record(store_, next_, end_, header_->seq);
  if (!record) {
    done_ = true;
    return std::nullopt;
  }
  next_ += record->stored_size();
  return record;
}

}  // namespace nanoer
