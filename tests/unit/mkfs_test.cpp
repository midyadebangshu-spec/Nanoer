#include "nanoer/mkfs.h"

#include <gtest/gtest.h>

#include "nanoer/error.h"
#include "nanoer/io.h"
#include "nanoer/segment.h"

using namespace nanoer;

TEST(Mkfs, DefaultGeometry) {
  MemStore store(1ull << 30);
  Superblock sb = make_filesystem(store, {});
  EXPECT_EQ(sb.segment_size(), 64u << 20);
  EXPECT_EQ(sb.segment_count, ((1ull << 30) - kDataStart) / (64 << 20));
  EXPECT_EQ(sb.chunk_size(), 128u << 10);
  EXPECT_EQ(sb.default_codec, CodecId::zstd);
  EXPECT_EQ(sb.default_level, 1);
  // The initial checkpoint sits at the start of segment 0, the first segment.
  EXPECT_EQ(sb.checkpoint_offset, kDataStart + kSegmentHeaderSize);
  EXPECT_EQ(sb.checkpoint_seq, 1u);
  EXPECT_EQ(sb.next_segment_seq, 2u);
  ASSERT_TRUE(read_segment_header(store, sb, 0));
  EXPECT_LE(sb.segment_offset(sb.segment_count), store.size());
  EXPECT_EQ(load_superblock(store).sb.uuid, sb.uuid);
}

TEST(Mkfs, OptionsAreRecorded) {
  MemStore store(64 << 20);
  Uuid uuid = random_uuid();
  make_filesystem(store, {.segment_shift = 22, .chunk_shift = 14, .codec = CodecId::lz4,
                          .level = 9, .label = "games", .uuid = uuid});
  Superblock sb = load_superblock(store).sb;
  EXPECT_EQ(sb.segment_size(), 4u << 20);
  EXPECT_EQ(sb.chunk_size(), 16u << 10);
  EXPECT_EQ(sb.default_codec, CodecId::lz4);
  EXPECT_EQ(sb.default_level, 9);
  EXPECT_EQ(sb.label, "games");
  EXPECT_EQ(sb.uuid, uuid);
}

TEST(Mkfs, RejectsBadOptions) {
  MemStore store(64 << 20);
  EXPECT_THROW(make_filesystem(store, {.segment_shift = 19}), Error);
  EXPECT_THROW(make_filesystem(store, {.segment_shift = 31}), Error);
  EXPECT_THROW(make_filesystem(store, {.chunk_shift = 11}), Error);
  EXPECT_THROW(make_filesystem(store, {.segment_shift = 20, .chunk_shift = 20}), Error);
  EXPECT_THROW(make_filesystem(store, {.segment_shift = 20, .codec = CodecId::lz4, .level = 13}),
               Error);
  EXPECT_THROW(make_filesystem(store, {.segment_shift = 20, .label = std::string(64, 'x')}), Error);
}

TEST(Mkfs, RejectsTooSmallStore) {
  MemStore store(min_store_size(20) - 1);
  EXPECT_THROW(make_filesystem(store, {.segment_shift = 20}), Error);
  MemStore just_enough(min_store_size(20));
  EXPECT_NO_THROW(make_filesystem(just_enough, {.segment_shift = 20}));
}

TEST(Mkfs, ReformatMakesOldSegmentsFree) {
  MemStore store(64 << 20);
  Superblock old = make_filesystem(store, {.segment_shift = 20});
  SegmentWriter writer(store, old, 5, 9);
  writer.flush();
  ASSERT_TRUE(read_segment_header(store, old, 5));

  Superblock fresh = make_filesystem(store, {.segment_shift = 20});
  EXPECT_NE(fresh.uuid, old.uuid);
  EXPECT_FALSE(read_segment_header(store, fresh, 5)) << "segment from the old filesystem counted";
}
