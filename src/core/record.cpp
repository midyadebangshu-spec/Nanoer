#include "nanoer/record.h"

#include <cstring>

#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/io.h"

namespace nanoer {
namespace {

// Common record header layout; see docs/format.md.
namespace off {
constexpr std::size_t magic = 0;
constexpr std::size_t type = 4;
constexpr std::size_t flags = 5;
constexpr std::size_t header_len = 6;
constexpr std::size_t payload_len = 8;
constexpr std::size_t payload_crc = 12;
constexpr std::size_t seq = 16;
constexpr std::size_t header_crc = 28;
}  // namespace off

// Data chunk header layout, relative to the end of the common header.
namespace chunk_off {
constexpr std::size_t codec = 0;
constexpr std::size_t uncompressed_size = 4;
constexpr std::size_t xxh3 = 8;
constexpr std::size_t inode = 16;
constexpr std::size_t file_offset = 24;
constexpr std::size_t blake3 = 32;
}  // namespace chunk_off

std::uint32_t header_crc(ByteView header) {
  // The CRC field itself counts as zero.
  std::uint32_t crc = crc32c(header.first(off::header_crc));
  static constexpr std::uint8_t kZero[4] = {};
  crc = crc32c(ByteView(kZero, 4), crc);
  return crc32c(header.subspan(off::header_crc + 4), crc);
}

[[noreturn]] void corrupt_at(std::uint64_t offset, const std::string& what) {
  throw Error(Errc::corrupt, "record at offset " + std::to_string(offset) + ": " + what);
}

}  // namespace

Bytes encode_record(RecordType type, std::uint8_t flags, std::uint64_t seq, ByteView extra,
                    ByteView payload) {
  std::size_t header_len = kRecordHeaderSize + extra.size();
  if (header_len % kRecordAlignment != 0 || header_len > kMaxRecordHeaderSize) {
    throw Error(Errc::invalid_argument, "bad record header size");
  }
  if (payload.size() > UINT32_MAX) throw Error(Errc::invalid_argument, "record payload too large");

  Bytes b(align_up(header_len + payload.size(), kRecordAlignment), 0);
  store_le(b.data() + off::magic, kRecordMagic);
  b[off::type] = static_cast<std::uint8_t>(type);
  b[off::flags] = flags;
  store_le(b.data() + off::header_len, static_cast<std::uint16_t>(header_len));
  store_le(b.data() + off::payload_len, static_cast<std::uint32_t>(payload.size()));
  store_le(b.data() + off::payload_crc, crc32c(payload));
  store_le(b.data() + off::seq, seq);
  if (!extra.empty()) std::memcpy(b.data() + kRecordHeaderSize, extra.data(), extra.size());
  if (!payload.empty()) std::memcpy(b.data() + header_len, payload.data(), payload.size());
  store_le(b.data() + off::header_crc, header_crc(ByteView(b.data(), header_len)));
  return b;
}

std::optional<Record> try_read_record_header(Store& store, std::uint64_t offset,
                                             std::uint64_t limit, std::uint64_t seq) {
  if (offset % kRecordAlignment != 0 || offset > limit || limit - offset < kRecordHeaderSize) {
    return std::nullopt;
  }
  std::uint8_t head[kRecordHeaderSize];
  store.read(offset, MutableByteView(head, sizeof head));

  if (load_le<std::uint32_t>(head + off::magic) != kRecordMagic) return std::nullopt;
  RecordHeader h;
  h.type = static_cast<RecordType>(head[off::type]);
  h.flags = head[off::flags];
  h.header_len = load_le<std::uint16_t>(head + off::header_len);
  h.payload_len = load_le<std::uint32_t>(head + off::payload_len);
  h.payload_crc = load_le<std::uint32_t>(head + off::payload_crc);
  h.seq = load_le<std::uint64_t>(head + off::seq);
  // A record from an earlier use of this segment can look intact; its
  // sequence number gives it away.
  if (h.seq != seq) return std::nullopt;
  if (h.header_len < kRecordHeaderSize || h.header_len > kMaxRecordHeaderSize ||
      h.header_len % kRecordAlignment != 0) {
    return std::nullopt;
  }
  if (std::uint64_t{h.header_len} + h.payload_len > limit - offset) return std::nullopt;

  Bytes header(head, head + sizeof head);
  header.resize(h.header_len);
  if (h.header_len > kRecordHeaderSize) {
    store.read(offset + kRecordHeaderSize,
               MutableByteView(header.data() + kRecordHeaderSize, h.header_len - kRecordHeaderSize));
  }
  if (load_le<std::uint32_t>(head + off::header_crc) != header_crc(header)) return std::nullopt;

  Record r;
  r.offset = offset;
  r.header = h;
  r.extra.assign(header.begin() + kRecordHeaderSize, header.end());
  return r;
}

std::optional<Record> try_read_record(Store& store, std::uint64_t offset, std::uint64_t limit,
                                      std::uint64_t seq) {
  std::optional<Record> r = try_read_record_header(store, offset, limit, seq);
  if (!r) return std::nullopt;
  r->payload = store.read(offset + r->header.header_len, r->header.payload_len);
  if (crc32c(r->payload) != r->header.payload_crc) return std::nullopt;
  return r;
}

Record read_record(Store& store, std::uint64_t offset, std::uint64_t limit, std::uint64_t seq) {
  auto r = try_read_record(store, offset, limit, seq);
  if (!r) corrupt_at(offset, "missing or damaged");
  return std::move(*r);
}

Bytes encode_data_chunk_record(std::uint64_t seq, const EncodedChunk& chunk, std::uint64_t inode,
                               std::uint64_t file_offset, const std::optional<Blake3Hash>& blake3) {
  Bytes extra(blake3 ? 64 : 32, 0);
  extra[chunk_off::codec] = static_cast<std::uint8_t>(chunk.codec);
  store_le(extra.data() + chunk_off::uncompressed_size, chunk.uncompressed_size);
  store_le(extra.data() + chunk_off::xxh3, chunk.xxh3);
  store_le(extra.data() + chunk_off::inode, inode);
  store_le(extra.data() + chunk_off::file_offset, file_offset);
  std::uint8_t flags = 0;
  if (blake3) {
    std::memcpy(extra.data() + chunk_off::blake3, blake3->data(), blake3->size());
    flags |= data_chunk_flags::has_blake3;
  }
  return encode_record(RecordType::data_chunk, flags, seq, extra, chunk.payload);
}

DataChunkInfo parse_data_chunk(const Record& r) {
  if (r.header.type != RecordType::data_chunk) corrupt_at(r.offset, "not a data chunk");
  if (r.header.flags & ~data_chunk_flags::known) corrupt_at(r.offset, "unknown data chunk flags");
  bool has_id = r.header.flags & data_chunk_flags::has_blake3;
  if (r.extra.size() != (has_id ? 64u : 32u)) corrupt_at(r.offset, "bad data chunk header size");

  const std::uint8_t* e = r.extra.data();
  auto codec = codec_from_id(e[chunk_off::codec]);
  if (!codec) corrupt_at(r.offset, "unknown codec");
  DataChunkInfo info;
  info.codec = *codec;
  info.uncompressed_size = load_le<std::uint32_t>(e + chunk_off::uncompressed_size);
  info.xxh3 = load_le<std::uint64_t>(e + chunk_off::xxh3);
  info.inode = load_le<std::uint64_t>(e + chunk_off::inode);
  info.file_offset = load_le<std::uint64_t>(e + chunk_off::file_offset);
  if (has_id) {
    Blake3Hash id;
    std::memcpy(id.data(), e + chunk_off::blake3, id.size());
    info.blake3 = id;
  }
  if (info.uncompressed_size > (std::uint32_t{1} << kMaxChunkShift)) {
    corrupt_at(r.offset, "chunk larger than the maximum chunk size");
  }
  if (info.codec == CodecId::none && r.header.payload_len != info.uncompressed_size) {
    corrupt_at(r.offset, "stored chunk size mismatch");
  }
  return info;
}

Bytes decode_data_chunk(const Record& r) {
  DataChunkInfo info = parse_data_chunk(r);
  try {
    return decode_chunk(info.codec, r.payload, info.uncompressed_size, info.xxh3);
  } catch (const Error& e) {
    if (e.code() == Errc::corrupt) corrupt_at(r.offset, e.what());
    throw;
  }
}

}  // namespace nanoer
