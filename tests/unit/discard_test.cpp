// Discard (docs/M6_PLAN.md §5): segments that become free are given back to
// the layer below, and nothing that is still needed ever is.

#include <filesystem>
#include <map>
#include <random>
#include <string>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"
#include "nanoer/segment.h"
#include "test_access.h"

using namespace nanoer;

namespace {

constexpr std::uint64_t kChunk = 16 << 10;

Filesystem::Options options(bool discard = true) {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 1;
  o.discard = discard;
  return o;
}

Bytes noise(std::uint64_t seed, std::size_t n) {
  std::mt19937_64 rng(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

// Files of incompressible data, then most of them deleted and the space
// collected: whole segments become free.
std::map<InodeId, Bytes> fill_and_free(Filesystem& fs) {
  std::map<InodeId, Bytes> kept;
  std::vector<InodeId> all;
  for (int i = 0; i < 40; ++i) {
    InodeId f = fs.mknod(kRootInode, "f" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
    Bytes data = noise(static_cast<std::uint64_t>(i), 12 * kChunk);
    fs.write(f, 0, data);
    all.push_back(f);
    if (i % 8 == 0) kept[f] = data;
  }
  fs.sync();
  for (int i = 0; i < 40; ++i) {
    if (i % 8 != 0) fs.unlink(kRootInode, "f" + std::to_string(i));
  }
  fs.sync();
  fs.collect_garbage(0.97);
  // Freed segments become free two checkpoints after they emptied.
  FilesystemTestAccess::checkpoint(fs);
  FilesystemTestAccess::checkpoint(fs);
  return kept;
}

void expect_contents(Filesystem& fs, const std::map<InodeId, Bytes>& files) {
  for (const auto& [ino, data] : files) {
    Bytes got(data.size());
    ASSERT_EQ(fs.read(ino, 0, got), data.size());
    ASSERT_TRUE(got == data) << "inode " << ino;
  }
}

std::uint64_t written_segments(Store& store) {
  Superblock sb = load_superblock(store).sb;
  std::uint64_t n = 0;
  for (std::uint64_t i = 0; i < sb.segment_count; ++i) n += read_segment_header(store, sb, i).has_value();
  return n;
}

}  // namespace

TEST(Discard, FreedSegmentsAreGivenBackAndReusedSafely) {
  MemStore store(32 << 20);
  make_filesystem(store, {.segment_shift = 20, .chunk_shift = 14});
  std::map<InodeId, Bytes> kept;
  {
    auto fs = Filesystem::mount(store, options());
    kept = fill_and_free(*fs);
    std::uint64_t before = written_segments(store);
    ASSERT_GT(fs->info().discard_queued, 0u);
    std::size_t n = fs->discard_freed();
    EXPECT_GT(n, 0u);
    EXPECT_EQ(fs->info().discard_queued, 0u);
    EXPECT_EQ(fs->info().segments_discarded, n);
    EXPECT_EQ(written_segments(store), before - n) << "a discarded segment reads as zeros";
    expect_contents(*fs, kept);
    // The log takes discarded segments again as it needs them.
    for (int i = 0; i < 30; ++i) {
      InodeId f = fs->mknod(kRootInode, "g" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
      Bytes data = noise(100 + static_cast<std::uint64_t>(i), 12 * kChunk);
      fs->write(f, 0, data);
      kept[f] = data;
    }
    fs->sync();
    expect_contents(*fs, kept);
  }
  FsckReport r = fsck(store, {.repair = false, .deep = true});
  EXPECT_EQ(r.exit_code(), 0);
  auto fs = Filesystem::mount(store, options());
  expect_contents(*fs, kept);
}

TEST(Discard, OffMeansNothingIsGivenBack) {
  MemStore store(32 << 20);
  make_filesystem(store, {.segment_shift = 20, .chunk_shift = 14});
  auto fs = Filesystem::mount(store, options(false));
  fill_and_free(*fs);
  EXPECT_EQ(fs->info().discard_queued, 0u);
  EXPECT_EQ(fs->discard_freed(), 0u);
}

TEST(Discard, SegmentsFreedBeforeACrashAreGivenBackAtTheNextMount) {
  MemStore store(32 << 20);
  make_filesystem(store, {.segment_shift = 20, .chunk_shift = 14});
  std::map<InodeId, Bytes> kept;
  std::size_t queued = 0;
  {
    auto fs = Filesystem::mount(store, options());
    kept = fill_and_free(*fs);
    queued = fs->info().discard_queued;
    ASSERT_GT(queued, 0u);
    fs->abandon();  // a crash before the background got to them
  }
  auto fs = Filesystem::mount(store, options());
  // Found again from their intact headers (less the one the log continues in).
  EXPECT_GE(fs->info().discard_queued, queued - 1);
  fs->discard_freed();
  expect_contents(*fs, kept);
  fs.reset();
  // And once given back, they are not queued again.
  auto again = Filesystem::mount(store, options());
  EXPECT_EQ(again->info().discard_queued, 0u);
}

TEST(Discard, AnImageFileGetsItsSpaceBack) {
  // A real file: the holes show in its allocated size.
  std::string dir = ::testing::TempDir() + "nanoer-discard-" + std::to_string(::getpid());
  std::filesystem::create_directories(dir);
  std::string path = dir + "/fs.img";
  std::filesystem::remove(path);
  {
    auto store = FileStore::create_image(path, 32 << 20);
    make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14});
  }
  auto allocated = [&] {
    struct stat st {};
    EXPECT_EQ(::stat(path.c_str(), &st), 0);
    return static_cast<std::uint64_t>(st.st_blocks) * 512;
  };
  auto store = FileStore::open(path, FileStore::Mode::read_write);
  auto fs = Filesystem::mount(*store, options());
  std::map<InodeId, Bytes> kept = fill_and_free(*fs);
  std::uint64_t before = allocated();
  std::size_t n = fs->discard_freed();
  ASSERT_GT(n, 0u);
  std::uint64_t after = allocated();
  std::cout << "[ discard ] image file: " << before / 1024 << " KiB allocated, " << after / 1024 << " KiB after giving "
            << n << " segments of 1 MiB back\n";
  EXPECT_LE(after + n * (1 << 20) / 2, before) << "at least half of each discarded segment no longer allocated";
  expect_contents(*fs, kept);
  fs.reset();
  store.reset();
  std::filesystem::remove_all(dir);
}
