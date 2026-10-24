#include "nanoer/segment.h"

#include <random>

#include <gtest/gtest.h>

#include "nanoer/error.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;

namespace {

constexpr std::uint64_t kStoreSize = 16 << 20;

Bytes chunk_record(std::uint64_t seq, std::uint64_t inode, std::size_t size) {
  Bytes data(size);
  for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>((i * inode) % 251);
  return encode_data_chunk_record(seq, encode_chunk(data, CodecId::zstd, 1), inode, 0);
}

std::vector<Record> scan_all(Store& store, const Superblock& sb, std::uint64_t index) {
  SegmentScanner scanner(store, sb, index);
  std::vector<Record> out;
  while (auto r = scanner.next()) out.push_back(std::move(*r));
  return out;
}

}  // namespace

TEST(SegmentHeader, RoundTripAndChecks) {
  Uuid uuid = random_uuid();
  Bytes b = encode_segment_header({uuid, 9, 3, 77});
  ASSERT_EQ(b.size(), kSegmentHeaderSize);
  auto h = decode_segment_header(b, uuid, 3);
  ASSERT_TRUE(h);
  EXPECT_EQ(h->seq, 9u);
  EXPECT_EQ(h->created_ns, 77u);
  EXPECT_FALSE(decode_segment_header(b, random_uuid(), 3));  // another filesystem's
  EXPECT_FALSE(decode_segment_header(b, uuid, 4));           // misplaced
  b[30] ^= 1;
  EXPECT_FALSE(decode_segment_header(b, uuid, 3));
}

TEST(Segment, WriteThenScan) {
  MemStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  EXPECT_FALSE(SegmentScanner(store, sb, 2).valid());

  SegmentWriter writer(store, sb, 2, 7);
  std::vector<std::uint64_t> offsets;
  for (std::uint64_t i = 1; i <= 20; ++i) {
    auto at = writer.append(chunk_record(7, i, 5000));
    ASSERT_TRUE(at);
    offsets.push_back(*at);
  }
  writer.flush();

  std::vector<Record> records = scan_all(store, sb, 2);
  ASSERT_EQ(records.size(), 20u);
  for (std::size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(records[i].offset, offsets[i]);
    EXPECT_EQ(parse_data_chunk(records[i]).inode, i + 1);
  }
  SegmentScanner scanner(store, sb, 2);
  EXPECT_TRUE(scanner.valid());
  EXPECT_EQ(scanner.header().seq, 7u);
  while (scanner.next()) {
  }
  EXPECT_EQ(scanner.end_offset(), sb.segment_offset(2) + writer.used());
}

TEST(Segment, AppendStopsWhenFull) {
  MemStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  SegmentWriter writer(store, sb, 0, 1);
  Bytes big = encode_record(RecordType::metadata_log, 0, 1, {}, Bytes(300 << 10, 1));
  int appended = 0;
  while (writer.append(big)) ++appended;
  EXPECT_EQ(appended, 3);  // 3 x 300 KiB fit in 1 MiB minus the header; a 4th does not
  EXPECT_LT(writer.remaining(), big.size());
  writer.flush();
  EXPECT_EQ(scan_all(store, sb, 0).size(), 3u);
}

TEST(Segment, ReusedSegmentHidesOldRecords) {
  MemStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  {
    SegmentWriter first(store, sb, 1, 3);
    for (int i = 0; i < 10; ++i) first.append(chunk_record(3, 100, 2000));
    first.flush();
  }
  // GC frees the segment and the log reuses it with a newer sequence number.
  SegmentWriter second(store, sb, 1, 8);
  second.append(chunk_record(8, 200, 2000));
  second.flush();
  std::vector<Record> records = scan_all(store, sb, 1);
  ASSERT_EQ(records.size(), 1u) << "records from the earlier use leaked through";
  EXPECT_EQ(parse_data_chunk(records[0]).inode, 200u);
}

TEST(Segment, ScanStopsAtTornWrite) {
  FaultStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  SegmentWriter writer(store, sb, 0, 1);
  for (std::uint64_t i = 1; i <= 5; ++i) writer.append(chunk_record(1, i, 3000));
  writer.flush();
  // Five more records reach the store, but power fails before a flush.
  for (std::uint64_t i = 6; i <= 10; ++i) writer.append(chunk_record(1, i, 3000));
  writer.write_buffered();

  // Every crash keeps the five durable records, plus at most an intact
  // prefix of the rest, never a damaged record.
  bool saw_partial = false;
  for (std::uint64_t seed = 0; seed < 40; ++seed) {
    MemStore after(store.crash_image({.keep_probability = 0.7, .torn_writes = true, .seed = seed}));
    std::vector<Record> records = scan_all(after, sb, 0);
    ASSERT_GE(records.size(), 5u);
    ASSERT_LE(records.size(), 10u);
    saw_partial |= records.size() > 5 && records.size() < 10;
    for (std::size_t i = 0; i < records.size(); ++i) {
      EXPECT_EQ(parse_data_chunk(records[i]).inode, i + 1);
      EXPECT_NO_THROW(decode_data_chunk(records[i]));
    }
  }
  EXPECT_TRUE(saw_partial);
}

TEST(Segment, WriterValidatesArguments) {
  MemStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  EXPECT_THROW(SegmentWriter(store, sb, sb.segment_count, 1), Error);
  EXPECT_THROW(SegmentWriter(store, sb, 0, 0), Error);
  SegmentWriter writer(store, sb, 0, 1);
  EXPECT_THROW(writer.append(Bytes(7, 0)), Error);
}
