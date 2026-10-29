// nanoer upgrade (docs/M6_PLAN.md §4) on images made by the M2-M5 builds
// (tests/images): the same files afterwards, writable, the records GC
// could not find the user of given a list of users, and refusals that
// change nothing. A crash at every point of an upgrade: crash_test.cpp.

#include <sstream>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/checksum.h"
#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/journal.h"
#include "nanoer/upgrade.h"
#include "test_access.h"
#include "test_images.h"

using namespace nanoer;

namespace {

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 1;
  return o;
}

void dump_dir(Filesystem& fs, InodeId dir, const std::string& path, std::ostringstream& out) {
  for (const DirEntry& e : fs.list(dir)) {
    if (e.name == "." || e.name == "..") continue;
    Attr a = fs.getattr(e.ino);
    std::string p = path + "/" + e.name;
    out << p << " ino=" << a.ino << " mode=" << std::oct << a.mode << std::dec << " nlink=" << a.nlink
        << " size=" << a.size << " mtime=" << a.mtime_ns << " ctime=" << a.ctime_ns;
    for (const std::string& x : fs.listxattr(e.ino)) out << " xattr:" << x << "=" << fs.getxattr(e.ino, x).size();
    if (S_ISREG(a.mode)) {
      Bytes data(a.size);
      fs.read(e.ino, 0, data);
      out << " data=" << std::hex << xxh3_64(data) << std::dec;
    } else if (S_ISLNK(a.mode)) {
      out << " target=" << fs.readlink(e.ino);
    }
    out << "\n";
    if (S_ISDIR(a.mode)) dump_dir(fs, e.ino, p, out);
  }
}

std::string dump(Store& store, bool* read_only = nullptr) {
  auto fs = Filesystem::mount(store, options());
  if (read_only) *read_only = fs->read_only();
  std::ostringstream out;
  dump_dir(*fs, kRootInode, "", out);
  fs->abandon();
  return out.str();
}

}  // namespace

TEST(Upgrade, EveryOldFilesystemKeepsItsFilesAndBecomesWritable) {
  for (const std::string& name : kOldImages) {
    SCOPED_TRACE(name);
    MemStore store(load_test_image(name));
    bool read_only = false;
    std::string before = dump(store, &read_only);
    EXPECT_TRUE(read_only) << "an old filesystem mounts read-only";
    EXPECT_FALSE(has_metadata_tree(load_superblock(store).sb));

    UpgradeReport r = upgrade(store);
    EXPECT_FALSE(r.already);
    EXPECT_GT(r.inodes, 10u);
    Superblock sb = load_superblock(store).sb;
    EXPECT_TRUE(has_metadata_tree(sb) && has_shared_records(sb) && has_cold_data(sb));
    FsckReport check = fsck(store, {.repair = false, .deep = true});
    EXPECT_EQ(check.exit_code(), 0) << (check.errors.empty() ? "" : check.errors.front());
    EXPECT_EQ(dump(store, &read_only), before);
    EXPECT_FALSE(read_only);
    EXPECT_TRUE(upgrade(store).already) << "a second upgrade does nothing";

    // Writable, and everything still there after GC moved what it could.
    {
      auto fs = Filesystem::mount(store, options());
      InodeId f = fs->mknod(kRootInode, "new", S_IFREG | 0644, 0, {}).ino;
      fs->write(f, 0, as_bytes("written after the upgrade"));
      fs->collect_garbage(0.97);
      fs->unlink(kRootInode, "new");
    }
    EXPECT_EQ(dump(store), before);
    EXPECT_EQ(fsck(store, {.repair = false, .deep = true}).exit_code(), 0);
  }
}

TEST(Upgrade, RecordsWhoseHeaderNamesAnotherFileGetTheirUsers) {
  // M4 and M5 images hold dup.bin: a copy deduplicated against orig.bin,
  // which was then deleted. Its records' headers name orig.bin; without a
  // list of users GC could not find dup.bin as their user (the records
  // would stay live but never move, and their segments never empty).
  for (const std::string& name : {std::string("m4"), std::string("m5")}) {
    SCOPED_TRACE(name);
    MemStore store(load_test_image(name));
    UpgradeReport r = upgrade(store);
    EXPECT_EQ(r.named_users, 2u) << "dup.bin's two chunks";
    auto fs = Filesystem::mount(store, options());
    InodeId dup = fs->lookup(kRootInode, "dup.bin").ino;
    const Metadata& meta = FilesystemTestAccess::metadata(*fs);
    for (const auto& [index, ref] : FilesystemTestAccess::chunks(*fs, dup)) {
      std::optional<std::vector<ChunkUser>> users = meta.users(ref.offset);
      ASSERT_TRUE(users) << "chunk " << index;
      EXPECT_EQ(*users, (std::vector<ChunkUser>{{dup, index}}));
    }
  }
}

TEST(Upgrade, ADamagedFilesystemIsLeftAsItWas) {
  MemStore store(load_test_image("m3"));
  Superblock sb = load_superblock(store).sb;
  sb.state |= state_flags::errors;  // damage was seen once
  write_superblock(store, sb);
  Bytes before = store.image();
  try {
    upgrade(store);
    FAIL() << "upgraded a filesystem that recorded damage";
  } catch (const Error& e) {
    EXPECT_NE(std::string(e.what()).find("fsck"), std::string::npos) << e.what();
  }
  EXPECT_TRUE(store.image() == before);
}

TEST(Upgrade, WithoutRoomItSaysHowMuchAndChangesNothing) {
  // The M5 image, its free segments taken up by an imaginary checkpoint
  // too large to have room for its tree.
  Bytes image = load_test_image("m5");
  MemStore probe(image);
  Superblock sb = load_superblock(probe).sb;
  ReplayResult r = replay(probe, sb);
  std::uint64_t used = r.legacy->segments.size();
  // Shrink the store to the segments in use and two more.
  sb.segment_count = std::max<std::uint64_t>(used + 2, r.legacy->segments.rbegin()->first + 3);
  sb.store_size = sb.segment_offset(sb.segment_count);
  if (sb.store_size >= image.size()) GTEST_SKIP() << "the image is too full to shrink";
  image.resize(sb.store_size);
  MemStore store(image);
  write_superblock(store, sb);
  write_superblock(store, sb);
  Bytes before = store.image();
  try {
    upgrade(store);
    FAIL() << "upgraded without room";
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::no_space) << e.what();
    EXPECT_NE(std::string(e.what()).find("free segments"), std::string::npos) << e.what();
  }
  EXPECT_TRUE(store.image() == before);
}
