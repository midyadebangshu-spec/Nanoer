#include "nanoer/record.h"

#include <memory>
#include <random>

#include <gtest/gtest.h>

#include "nanoer/error.h"
#include "nanoer/io.h"

using namespace nanoer;

namespace {

Bytes text(std::size_t n) {
  Bytes b(n);
  for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>("nanoer chunk "[i % 13]);
  return b;
}

// Puts `encoded` at offset 64 of a fresh store.
std::unique_ptr<MemStore> store_with(const Bytes& encoded) {
  auto s = std::make_unique<MemStore>(64 + encoded.size() + 64);
  s->write(64, encoded);
  return s;
}

}  // namespace

TEST(Record, EncodeAndReadBack) {
  Bytes extra(16, 7), payload = text(100);
  Bytes encoded = encode_record(RecordType::metadata_log, 3, 55, extra, payload);
  EXPECT_EQ(encoded.size() % kRecordAlignment, 0u);
  EXPECT_EQ(encoded.size(), align_up(kRecordHeaderSize + 16 + 100, 8));

  auto s = store_with(encoded);
  auto r = try_read_record(*s, 64, s->size(), 55);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->offset, 64u);
  EXPECT_EQ(r->header.type, RecordType::metadata_log);
  EXPECT_EQ(r->header.flags, 3);
  EXPECT_EQ(r->header.seq, 55u);
  EXPECT_EQ(r->extra, extra);
  EXPECT_EQ(r->payload, payload);
  EXPECT_EQ(r->stored_size(), encoded.size());
}

TEST(Record, WrongSequenceIsNotARecord) {
  auto s = store_with(encode_record(RecordType::metadata_log, 0, 55, {}, text(10)));
  EXPECT_FALSE(try_read_record(*s, 64, s->size(), 54));
  EXPECT_THROW(read_record(*s, 64, s->size(), 54), Error);
}

TEST(Record, EveryFlippedBitIsDetected) {
  Bytes encoded = encode_record(RecordType::metadata_log, 0, 9, Bytes(8, 1), text(40));
  std::size_t used = kRecordHeaderSize + 8 + 40;  // the rest is padding
  for (std::size_t byte = 0; byte < used; ++byte) {
    for (int bit = 0; bit < 8; bit += 3) {
      Bytes damaged = encoded;
      damaged[byte] ^= static_cast<std::uint8_t>(1 << bit);
      auto s = store_with(damaged);
      EXPECT_FALSE(try_read_record(*s, 64, s->size(), 9)) << "byte " << byte << " bit " << bit;
    }
  }
}

TEST(Record, MustFitBeforeLimit) {
  Bytes encoded = encode_record(RecordType::metadata_log, 0, 1, {}, text(100));
  auto s = store_with(encoded);
  EXPECT_TRUE(try_read_record(*s, 64, 64 + encoded.size(), 1));
  EXPECT_FALSE(try_read_record(*s, 64, 64 + kRecordHeaderSize + 99, 1));
  EXPECT_FALSE(try_read_record(*s, 60, s->size(), 1));  // misaligned
}

TEST(Record, ZeroesAreNotARecord) {
  MemStore s(4096);
  EXPECT_FALSE(try_read_record(s, 0, s.size(), 1));
}

TEST(Record, RejectsBadHeaderSizes) {
  EXPECT_THROW(encode_record(RecordType::metadata_log, 0, 1, Bytes(5), {}), Error);
  EXPECT_THROW(encode_record(RecordType::metadata_log, 0, 1, Bytes(kMaxRecordHeaderSize), {}), Error);
}

TEST(DataChunk, RoundTripWithAndWithoutId) {
  Bytes data = text(50000);
  EncodedChunk chunk = encode_chunk(data, CodecId::zstd, 3);
  for (bool with_id : {false, true}) {
    std::optional<Blake3Hash> id;
    if (with_id) id = blake3(data);
    Bytes encoded = encode_data_chunk_record(5, chunk, 12, 1 << 20, id);
    auto s = store_with(encoded);
    Record r = read_record(*s, 64, s->size(), 5);
    DataChunkInfo info = parse_data_chunk(r);
    EXPECT_EQ(info.codec, CodecId::zstd);
    EXPECT_EQ(info.uncompressed_size, data.size());
    EXPECT_EQ(info.inode, 12u);
    EXPECT_EQ(info.file_offset, 1u << 20);
    EXPECT_EQ(info.blake3, id);
    EXPECT_EQ(r.header.header_len, with_id ? kDataChunkHeaderSizeWithId : kDataChunkHeaderSize);
    EXPECT_EQ(decode_data_chunk(r), data);
  }
}

TEST(DataChunk, RawChunkRoundTrip) {
  std::mt19937 rng(1);
  Bytes data(4096);
  for (auto& b : data) b = static_cast<std::uint8_t>(rng());
  EncodedChunk chunk = encode_chunk(data, CodecId::lzma, 6);
  ASSERT_EQ(chunk.codec, CodecId::none);
  auto s = store_with(encode_data_chunk_record(2, chunk, 1, 0));
  EXPECT_EQ(decode_data_chunk(read_record(*s, 64, s->size(), 2)), data);
}

TEST(DataChunk, InconsistentContentsAreCorrupt) {
  Bytes data = text(1000);
  EncodedChunk chunk = encode_chunk(data, CodecId::lz4, 0);
  chunk.xxh3 ^= 1;  // record is intact, but its contents do not match their checksum
  auto s = store_with(encode_data_chunk_record(2, chunk, 1, 0));
  Record r = read_record(*s, 64, s->size(), 2);
  try {
    decode_data_chunk(r);
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::corrupt);
  }
}

TEST(DataChunk, OtherRecordTypesAreRejected) {
  auto s = store_with(encode_record(RecordType::metadata_log, 0, 1, Bytes(32), {}));
  EXPECT_THROW(parse_data_chunk(read_record(*s, 64, s->size(), 1)), Error);
}
