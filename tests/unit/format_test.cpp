#include "nanoer/format.h"

#include <gtest/gtest.h>

#include "nanoer/checksum.h"
#include "nanoer/error.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;

namespace {

constexpr std::uint64_t kStoreSize = 64 << 20;

Superblock sample() {
  Superblock sb;
  sb.uuid = random_uuid();
  sb.generation = 7;
  sb.store_size = kStoreSize;
  sb.segment_shift = 20;
  sb.segment_count = (kStoreSize - kDataStart) >> 20;
  sb.created_ns = 1'700'000'000'000'000'000;
  sb.updated_ns = sb.created_ns + 5;
  sb.next_segment_seq = 42;
  sb.checkpoint_offset = kDataStart + 4096;
  sb.checkpoint_seq = 41;
  sb.default_codec = CodecId::lzma;
  sb.default_level = 6;
  sb.chunk_shift = 16;
  sb.label = "my disk";
  return sb;
}

// Re-seals a modified superblock so only the field checks can reject it.
Bytes reseal(Bytes block) {
  store_le(block.data() + kBlockSize - 4, crc32c(ByteView(block.data(), kBlockSize - 4)));
  return block;
}

}  // namespace

TEST(Superblock, EncodeDecodeRoundTrip) {
  Superblock sb = sample();
  Bytes block = encode_superblock(sb);
  ASSERT_EQ(block.size(), kBlockSize);
  DecodedSuperblock d = decode_superblock(block);
  ASSERT_EQ(d.state, DecodedSuperblock::State::valid) << d.reason;
  EXPECT_EQ(d.sb.uuid, sb.uuid);
  EXPECT_EQ(d.sb.generation, 7u);
  EXPECT_EQ(d.sb.segment_count, sb.segment_count);
  EXPECT_EQ(d.sb.next_segment_seq, 42u);
  EXPECT_EQ(d.sb.checkpoint_offset, sb.checkpoint_offset);
  EXPECT_EQ(d.sb.default_codec, CodecId::lzma);
  EXPECT_EQ(d.sb.default_level, 6);
  EXPECT_EQ(d.sb.chunk_size(), 64u << 10);
  EXPECT_EQ(d.sb.label, "my disk");
  EXPECT_EQ(encode_superblock(d.sb), block);
}

TEST(Superblock, FixedFieldPositions) {
  // Guards the documented layout in docs/format.md.
  Bytes b = encode_superblock(sample());
  EXPECT_EQ(std::string(b.begin(), b.begin() + 8), "NANOERFS");
  EXPECT_EQ(load_le<std::uint16_t>(b.data() + 8), kVersionMajor);
  EXPECT_EQ(load_le<std::uint32_t>(b.data() + 12), kBlockSize);
  EXPECT_EQ(load_le<std::uint64_t>(b.data() + 32), 7u);
  EXPECT_EQ(load_le<std::uint64_t>(b.data() + 112), 42u);
  EXPECT_EQ(b[136], 3);
  EXPECT_EQ(b[138], 16);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data() + 144)), "my disk");
}

TEST(Superblock, AnyFlippedBitIsDetected) {
  Bytes block = encode_superblock(sample());
  for (std::size_t i = 0; i < block.size(); i += 61) {
    Bytes damaged = block;
    damaged[i] ^= 0x10;
    EXPECT_EQ(decode_superblock(damaged).state, DecodedSuperblock::State::invalid) << i;
  }
}

TEST(Superblock, NewerVersionAndUnknownIncompatAreUnsupported) {
  Bytes block = encode_superblock(sample());
  Bytes newer = block;
  store_le<std::uint16_t>(newer.data() + 8, 2);
  EXPECT_EQ(decode_superblock(reseal(newer)).state, DecodedSuperblock::State::unsupported);

  Superblock sb = sample();
  sb.features_incompat = 1;
  EXPECT_EQ(decode_superblock(encode_superblock(sb)).state, DecodedSuperblock::State::unsupported);

  sb.features_incompat = 0;
  sb.features_compat = 1;  // safe to ignore
  EXPECT_EQ(decode_superblock(encode_superblock(sb)).state, DecodedSuperblock::State::valid);
}

TEST(Superblock, InconsistentFieldsAreInvalid) {
  auto check = [](auto mutate) {
    Superblock sb = sample();
    mutate(sb);
    return decode_superblock(encode_superblock(sb)).state;
  };
  using S = DecodedSuperblock::State;
  EXPECT_EQ(check([](Superblock& s) { s.segment_shift = 19; }), S::invalid);
  EXPECT_EQ(check([](Superblock& s) { s.chunk_shift = 25; }), S::invalid);
  EXPECT_EQ(check([](Superblock& s) { s.chunk_shift = 20; }), S::invalid);  // == segment size
  EXPECT_EQ(check([](Superblock& s) { s.segment_count += 1; }), S::invalid);
  EXPECT_EQ(check([](Superblock& s) { s.segment_count = 3; }), S::invalid);
  EXPECT_EQ(check([](Superblock& s) { s.default_codec = static_cast<CodecId>(9); }), S::invalid);
  EXPECT_EQ(check([](Superblock& s) { s.default_level = 10; }), S::invalid);  // lzma max is 9
  EXPECT_EQ(check([](Superblock& s) { s.checkpoint_offset = 100; }), S::invalid);
  EXPECT_EQ(check([](Superblock& s) { s.next_segment_seq = 0; }), S::invalid);
  EXPECT_EQ(check([](Superblock& s) { s.data_start = 4096; }), S::invalid);
}

TEST(Superblock, ReservedBytesAreIgnored) {
  // Found by fuzzing: a valid superblock may carry non-zero reserved bytes
  // (written by a later minor version); they must not change the fields.
  Superblock sb = sample();
  Bytes block = encode_superblock(sb);
  block[300] = 0x5a;                    // reserved area
  block[144 + sb.label.size() + 5] = 1;  // after the label's NUL
  DecodedSuperblock d = decode_superblock(reseal(block));
  ASSERT_EQ(d.state, DecodedSuperblock::State::valid) << d.reason;
  EXPECT_EQ(d.sb.label, sb.label);
  EXPECT_EQ(encode_superblock(d.sb), encode_superblock(sb));
}

TEST(Superblock, LabelLimit) {
  Superblock sb = sample();
  sb.label = std::string(63, 'x');
  EXPECT_EQ(decode_superblock(encode_superblock(sb)).sb.label, sb.label);
  sb.label.push_back('x');
  EXPECT_THROW(encode_superblock(sb), Error);
}

TEST(LoadSuperblock, PicksNewestAndAlternatesSlots) {
  MemStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  LoadedSuperblock loaded = load_superblock(store);
  EXPECT_EQ(loaded.sb.generation, 2u);
  EXPECT_EQ(loaded.slot, 0);
  EXPECT_FALSE(loaded.read_only);

  sb.next_segment_seq = 9;
  write_superblock(store, sb);
  loaded = load_superblock(store);
  EXPECT_EQ(loaded.sb.generation, 3u);
  EXPECT_EQ(loaded.slot, 1);
  EXPECT_EQ(loaded.sb.next_segment_seq, 9u);
}

TEST(LoadSuperblock, SurvivesTornSuperblockWrite) {
  FaultStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  const std::uint64_t original_seq = sb.next_segment_seq;
  // Write generation 3 without a flush, then lose power mid-write.
  sb.next_segment_seq = 9;
  ++sb.generation;
  store.write(kSuperblockOffsets[sb.generation % 2], encode_superblock(sb));
  for (std::uint64_t seed = 0; seed < 20; ++seed) {
    MemStore after(store.crash_image({.keep_probability = 1.0, .torn_writes = true, .seed = seed}));
    LoadedSuperblock loaded = load_superblock(after);
    // Either the new superblock made it whole, or the previous one is used.
    EXPECT_TRUE(loaded.sb.generation == 3 || loaded.sb.generation == 2);
    if (loaded.sb.generation == 2) {
      EXPECT_EQ(loaded.sb.next_segment_seq, original_seq);
    }
  }
}

TEST(LoadSuperblock, NeverFallsBackPastAnUnsupportedSuperblock) {
  MemStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  sb.features_incompat = 1 << 5;  // written by a hypothetical newer Nanoer
  write_superblock(store, sb);
  try {
    load_superblock(store);
    FAIL() << "fell back to an older superblock";
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::unsupported);
  }
}

TEST(LoadSuperblock, UnknownRoCompatMeansReadOnly) {
  MemStore store(kStoreSize);
  Superblock sb = make_filesystem(store, {.segment_shift = 20});
  sb.features_ro_compat = 1;
  write_superblock(store, sb);
  EXPECT_TRUE(load_superblock(store).read_only);
}

TEST(LoadSuperblock, RejectsBlankAndTruncatedStores) {
  MemStore blank(kStoreSize);
  EXPECT_THROW(load_superblock(blank), Error);

  MemStore store(kStoreSize);
  make_filesystem(store, {.segment_shift = 20});
  Bytes image = store.image();
  image.resize(kStoreSize / 2);
  MemStore truncated(image);
  try {
    load_superblock(truncated);
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::corrupt);
  }
}

TEST(Uuid, RandomVersion4) {
  Uuid a = random_uuid(), b = random_uuid();
  EXPECT_NE(a, b);
  EXPECT_EQ(a[6] >> 4, 4);
  EXPECT_EQ(a[8] >> 6, 2);
  std::string s = uuid_to_string(a);
  EXPECT_EQ(s.size(), 36u);
  EXPECT_EQ(s[8], '-');
  EXPECT_EQ(s[14], '4');
}
