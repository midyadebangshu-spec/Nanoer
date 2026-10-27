// nanoer stats: the figures add up, and are served on the root of a mount.

#include <random>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"
#include "nanoer/stats.h"

using namespace nanoer;

namespace {

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 1;
  return o;
}

Bytes noise(std::uint64_t seed, std::size_t n) {
  std::mt19937_64 rng(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

}  // namespace

TEST(Stats, EncodingRoundTrips) {
  SpaceStats s;
  s.files = 3;
  s.logical_bytes = 1ull << 40;
  s.dedup = true;
  s.gc_damaged = 7;
  s.by_codec["zstd"] = {5, 600, 300};
  SpaceStats back = decode_stats(encode_stats(s));
  EXPECT_EQ(encode_stats(back), encode_stats(s));
  EXPECT_EQ(back.by_codec.at("zstd").stored_bytes, 300u);
  EXPECT_THROW(decode_stats("files=lots\n"), Error);
}

TEST(Stats, TheFiguresAddUp) {
  MemStore store(64 << 20);
  make_filesystem(store, {.segment_shift = 20, .chunk_shift = 14});
  constexpr std::size_t kChunk = 16 << 10;
  auto fs = Filesystem::mount(store, options());
  Bytes random = noise(1, 10 * kChunk);
  Bytes text(10 * kChunk);
  // Compressible, but no two chunks alike.
  for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<std::uint8_t>("nanoer "[i % 7] + i / kChunk);
  auto file = [&](const std::string& name) { return fs->mknod(kRootInode, name, S_IFREG | 0644, 0, {}).ino; };
  fs->write(file("random"), 0, random);
  fs->write(file("text"), 0, text);
  fs->write(file("copy"), 0, random);                      // dedup: 10 chunks
  fs->write(file("sparse"), 40 * kChunk, Bytes(100, 'x'));  // a hole of 40 chunks
  for (int i = 0; i < 5; ++i) fs->write(file("small" + std::to_string(i)), 0, Bytes(500 + i, static_cast<std::uint8_t>('a' + i)));
  fs->sync();

  SpaceStats s = fs->space_stats(true);
  EXPECT_EQ(s.files, 9u);
  EXPECT_EQ(s.logical_bytes, 3 * random.size() + 40 * kChunk + 100 + 5 * 500 + 10);
  EXPECT_EQ(s.saved_by_holes(), 40 * kChunk);
  EXPECT_EQ(s.dedup_bytes, random.size());
  EXPECT_EQ(s.shared_records, 10u);
  EXPECT_EQ(s.packs, 1u);
  EXPECT_EQ(s.packed_files, 5u);
  EXPECT_GT(s.saved_by_compression(), text.size() / 2);  // the text compresses
  // stored = logical - holes - dedup - compression + dead space in packs
  EXPECT_EQ(s.stored_bytes, s.logical_bytes - s.saved_by_holes() - s.dedup_bytes - s.saved_by_compression() +
                                s.pack_dead_bytes);
  EXPECT_EQ(s.stored_bytes, s.live_bytes);  // the space accounting agrees
  std::uint64_t by_codec = 0;
  for (const auto& [codec, c] : s.by_codec) by_codec += c.stored_bytes;
  EXPECT_EQ(by_codec, s.stored_bytes);
  EXPECT_TRUE(s.dedup);
  EXPECT_GT(s.metadata_bytes, 0u);

  // Served on the root, as nanoerfs does through getxattr.
  Bytes value = fs->getxattr(kRootInode, kStatsXattr);
  SpaceStats served = decode_stats(std::string(value.begin(), value.end()));
  EXPECT_EQ(served.stored_bytes, s.stored_bytes);
  EXPECT_TRUE(served.by_codec.empty());  // not over the mount: too slow
  // The prefix is Nanoer's own.
  EXPECT_THROW(fs->setxattr(kRootInode, "user.nanoer.color", as_bytes("max"), 0), Error);
  EXPECT_THROW(fs->setxattr(kRootInode, kStatsXattr, as_bytes("x"), 0), Error);
  EXPECT_THROW(fs->getxattr(fs->lookup(kRootInode, "text").ino, kStatsXattr), Error);
}
