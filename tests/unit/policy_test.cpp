// Folder policies: the codec each one stores new data with, its cold form,
// inheritance by new files and folders, invalid values, and filesystems
// from before cold data (docs/M5_PLAN.md §4).

#include <map>
#include <random>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"
#include "nanoer/policy.h"
#include "nanoer/record.h"
#include "test_access.h"

using namespace nanoer;

namespace {

constexpr std::uint64_t kChunk = 128 << 10;

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 2;
  return o;
}

std::unique_ptr<MemStore> make_store(bool cold_data = true) {
  auto store = std::make_unique<MemStore>(96 << 20);
  make_filesystem(*store, {.segment_shift = 22, .cold_data = cold_data});
  return store;
}

Bytes text(std::uint64_t seed, std::size_t n) {
  std::mt19937_64 rng(seed);
  std::string s;
  while (s.size() < n) s += "line " + std::to_string(rng() % 5000) + " of the log: status ok, user " + std::to_string(rng() % 97) + "\n";
  s.resize(n);
  return Bytes(s.begin(), s.end());
}

void set_policy(Filesystem& fs, InodeId ino, std::string_view value) {
  fs.setxattr(ino, kPolicyXattr, ByteView(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()), 0);
}

std::string policy(Filesystem& fs, InodeId ino) {
  Bytes v = fs.getxattr(ino, kPolicyXattr);
  return std::string(v.begin(), v.end());
}

std::map<std::uint64_t, ChunkRef> chunks(Filesystem& fs, InodeId ino) { return FilesystemTestAccess::chunks(fs, ino); }

// The codec of the record behind a file's chunk.
CodecId codec_of(Filesystem& fs, Store& store, InodeId ino, std::uint64_t index = 0) {
  const Metadata& m = FilesystemTestAccess::metadata(fs);
  const ChunkRef ref = chunks(fs, ino).at(index);
  std::uint64_t seg = m.segment_of(ref.offset);
  const Superblock& sb = fs.superblock();
  auto r = try_read_record_header(store, ref.offset, sb.segment_offset(seg) + sb.segment_size(), m.segments.at(seg).seq);
  return parse_data_chunk(*r).codec;
}

}  // namespace

TEST(Policy, NamesAndDurations) {
  for (Policy p : {Policy::none, Policy::fast, Policy::balanced, Policy::max}) {
    EXPECT_EQ(parse_policy(policy_name(p)), p);
  }
  EXPECT_FALSE(parse_policy("Max"));
  EXPECT_FALSE(parse_policy("max\n"));
  EXPECT_FALSE(parse_policy(""));
  EXPECT_EQ(parse_duration("7d"), 7 * 86400);
  EXPECT_EQ(parse_duration("12h"), 12 * 3600);
  EXPECT_EQ(parse_duration("30m"), 1800);
  EXPECT_EQ(parse_duration("90s"), 90);
  EXPECT_EQ(parse_duration("0"), 0);
  EXPECT_EQ(parse_duration("never"), -1);
  EXPECT_FALSE(parse_duration("7w"));
  EXPECT_FALSE(parse_duration("d"));
  EXPECT_FALSE(parse_duration("99999999999999999999"));
  // When data counts as cold: never for none and fast; max after a day at most.
  EXPECT_EQ(cold_after_s(Policy::balanced, 7 * 86400), 7 * 86400);
  EXPECT_EQ(cold_after_s(Policy::max, 7 * 86400), 86400);
  EXPECT_EQ(cold_after_s(Policy::max, 60), 60);
  EXPECT_FALSE(cold_after_s(Policy::none, 0));
  EXPECT_FALSE(cold_after_s(Policy::fast, 0));
  EXPECT_FALSE(cold_after_s(Policy::balanced, -1));
}

TEST(Policy, EachStoresNewDataItsWay) {
  auto store = make_store();
  auto fs = Filesystem::mount(*store, options());
  std::map<std::string, InodeId> files;
  int seed = 0;
  for (const char* p : {"none", "fast", "balanced", "max"}) {
    InodeId dir = fs->mkdir(kRootInode, p, 0755, {}).ino;
    set_policy(*fs, dir, p);
    InodeId f = fs->mknod(dir, "data", S_IFREG | 0644, 0, {}).ino;
    fs->write(f, 0, text(++seed, 3 * kChunk));
    files[p] = f;
  }
  InodeId plain = fs->mknod(kRootInode, "plain", S_IFREG | 0644, 0, {}).ino;
  fs->write(plain, 0, text(9, 3 * kChunk));
  fs->sync();
  EXPECT_EQ(codec_of(*fs, *store, files["none"]), CodecId::none);
  EXPECT_EQ(codec_of(*fs, *store, files["fast"]), CodecId::lz4);
  EXPECT_EQ(codec_of(*fs, *store, files["balanced"]), CodecId::zstd);
  EXPECT_EQ(codec_of(*fs, *store, files["max"]), CodecId::zstd);
  EXPECT_EQ(codec_of(*fs, *store, plain), CodecId::zstd);

  // Cold: none and fast stay as they are.
  fs->recompress_cold(0);
  auto cold = [&](InodeId f) {
    for (const auto& [index, ref] : chunks(*fs, f)) {
      if (!ref.cold) return false;
    }
    return true;
  };
  EXPECT_FALSE(cold(files["none"]));
  EXPECT_FALSE(cold(files["fast"]));
  EXPECT_TRUE(cold(files["balanced"]));
  EXPECT_TRUE(cold(files["max"]));
  EXPECT_TRUE(cold(plain));
  EXPECT_EQ(codec_of(*fs, *store, files["none"]), CodecId::none);
  EXPECT_EQ(codec_of(*fs, *store, files["fast"]), CodecId::lz4);
  EXPECT_FALSE(chunks(*fs, files["max"]).at(0).packed) << "each chunk on its own";

  // Space by policy.
  SpaceStats s = fs->space_stats();
  EXPECT_EQ(s.by_policy["none"].files, 1u);
  EXPECT_EQ(s.by_policy["max"].logical_bytes, 3 * kChunk);
  EXPECT_EQ(s.by_policy["balanced"].files, 2u);  // the default included
  EXPECT_EQ(decode_stats(encode_stats(s)).by_policy["max"].logical_bytes, 3 * kChunk);
  EXPECT_GT(s.cold_bytes, 0u);
  fs.reset();
  EXPECT_EQ(fsck(*store, {.repair = false, .deep = true}).exit_code(), 0);
}

TEST(Policy, NewFilesAndFoldersInheritIt) {
  auto store = make_store();
  {
    auto fs = Filesystem::mount(*store, options());
    InodeId top = fs->mkdir(kRootInode, "top", 0755, {}).ino;
    InodeId before = fs->mknod(top, "before", S_IFREG | 0644, 0, {}).ino;
    set_policy(*fs, top, "max");
    InodeId sub = fs->mkdir(top, "sub", 0755, {}).ino;
    InodeId deep = fs->mknod(sub, "deep", S_IFREG | 0644, 0, {}).ino;
    InodeId link = fs->symlink(top, "link", "deep", {}).ino;
    EXPECT_EQ(policy(*fs, sub), "max");
    EXPECT_EQ(policy(*fs, deep), "max");
    EXPECT_THROW(fs->getxattr(before, kPolicyXattr), Error) << "only what is created afterwards";
    EXPECT_THROW(fs->getxattr(link, kPolicyXattr), Error) << "symlinks hold no data";
    // A changed folder changes what is created from then on.
    set_policy(*fs, sub, "fast");
    EXPECT_EQ(policy(*fs, fs->mknod(sub, "later", S_IFREG | 0644, 0, {}).ino), "fast");
    EXPECT_EQ(policy(*fs, deep), "max");
    fs->removexattr(sub, kPolicyXattr);
    EXPECT_THROW(fs->getxattr(fs->mknod(sub, "plain", S_IFREG | 0644, 0, {}).ino, kPolicyXattr), Error);
  }
  auto fs = Filesystem::mount(*store, options());  // kept across a remount
  InodeId top = fs->lookup(kRootInode, "top").ino;
  EXPECT_EQ(policy(*fs, top), "max");
  EXPECT_EQ(policy(*fs, fs->lookup(fs->lookup(top, "sub").ino, "deep").ino), "max");
}

TEST(Policy, InvalidValuesAndOtherNanoerNamesAreRefused) {
  auto store = make_store();
  auto fs = Filesystem::mount(*store, options());
  InodeId dir = fs->mkdir(kRootInode, "d", 0755, {}).ino;
  for (std::string_view bad : {"", "Max", "maximum", "max\n", "zstd"}) {
    try {
      set_policy(*fs, dir, bad);
      ADD_FAILURE() << "accepted '" << bad << "'";
    } catch (const Error& e) {
      EXPECT_EQ(e.sys_errno(), EINVAL) << bad;
    }
  }
  try {
    fs->setxattr(dir, "user.nanoer.other", Bytes{1}, 0);
    ADD_FAILURE() << "user.nanoer.other was accepted";
  } catch (const Error& e) {
    EXPECT_EQ(e.sys_errno(), EPERM);
  }
  EXPECT_THROW(fs->getxattr(dir, kPolicyXattr), Error);
}

TEST(Policy, SmallFilesArePackedOnlyWithTheirOwnPolicy) {
  auto store = make_store();
  auto fs = Filesystem::mount(*store, options());
  std::map<std::string, std::vector<InodeId>> files;
  std::uint64_t seed = 0;  // all different: none is deduplicated
  for (const char* p : {"none", "fast", "balanced"}) {
    InodeId dir = fs->mkdir(kRootInode, p, 0755, {}).ino;
    set_policy(*fs, dir, p);
    for (int i = 0; i < 4; ++i) {
      InodeId f = fs->mknod(dir, "s" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
      fs->write(f, 0, text(++seed, 2000));
      files[p].push_back(f);
    }
  }
  fs->sync();
  for (InodeId f : files["none"]) EXPECT_FALSE(chunks(*fs, f).at(0).packed);
  std::set<std::uint64_t> fast, balanced;
  for (InodeId f : files["fast"]) fast.insert(chunks(*fs, f).at(0).offset);
  for (InodeId f : files["balanced"]) balanced.insert(chunks(*fs, f).at(0).offset);
  EXPECT_EQ(fast.size(), 1u);
  EXPECT_EQ(balanced.size(), 1u);
  EXPECT_NE(*fast.begin(), *balanced.begin());
  EXPECT_EQ(codec_of(*fs, *store, files["fast"][0]), CodecId::lz4);
}

TEST(Policy, WorksOnFilesystemsWithoutColdData) {
  auto store = make_store(false);
  auto fs = Filesystem::mount(*store, options());
  InodeId dir = fs->mkdir(kRootInode, "d", 0755, {}).ino;
  set_policy(*fs, dir, "fast");
  InodeId f = fs->mknod(dir, "f", S_IFREG | 0644, 0, {}).ino;
  fs->write(f, 0, text(1, kChunk));
  fs->sync();
  EXPECT_EQ(codec_of(*fs, *store, f), CodecId::lz4);
  fs.reset();
  EXPECT_EQ(fsck(*store, {.repair = false, .deep = true}).exit_code(), 0);
}
