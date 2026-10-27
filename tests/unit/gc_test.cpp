// Garbage collection: victim choice, moving live data and freeing segments,
// rewrites that never fill the store, and the races between GC and writes
// (docs/M4_PLAN.md §3).

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <thread>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/gc.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"
#include "nanoer/segment.h"

#include "test_access.h"

using namespace nanoer;

namespace {

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 1;
  o.safe_space = 1 << 20;
  return o;
}

constexpr std::uint64_t kChunk = 16 << 10;

// 1 MiB segments, 16 KiB chunks, stored raw: sizes are predictable.
std::unique_ptr<MemStore> make_store(std::uint64_t size, bool shared_records = true) {
  auto store = std::make_unique<MemStore>(size);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14, .codec = CodecId::none, .level = 0,
                           .shared_records = shared_records});
  return store;
}

Bytes noise(std::mt19937_64& rng, std::size_t n) {
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

// Files with known contents, kept in memory to check against.
struct Files {
  std::map<InodeId, Bytes> data;

  void write(Filesystem& fs, InodeId ino, std::uint64_t at, ByteView bytes) {
    fs.write(ino, at, bytes);
    Bytes& d = data[ino];
    if (d.size() < at + bytes.size()) d.resize(at + bytes.size());
    std::copy(bytes.begin(), bytes.end(), d.begin() + static_cast<std::ptrdiff_t>(at));
  }
  void check(Filesystem& fs) const {
    for (const auto& [ino, expected] : data) {
      Bytes got(expected.size());
      ASSERT_EQ(fs.read(ino, 0, got), got.size()) << "inode " << ino;
      ASSERT_TRUE(got == expected) << "contents of inode " << ino;
    }
  }
};

// Files whose every other chunk was rewritten: the segments that held the
// first versions are half dead.
Files fragment(Filesystem& fs, std::mt19937_64& rng, int files, std::uint64_t size) {
  Files f;
  for (int i = 0; i < files; ++i) {
    InodeId ino = fs.mknod(kRootInode, "f" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
    f.write(fs, ino, 0, noise(rng, size));
  }
  fs.sync();
  for (auto& [ino, d] : f.data) {
    for (std::uint64_t at = 0; at < size; at += 2 * kChunk) f.write(fs, ino, at, noise(rng, kChunk));
  }
  fs.sync();
  return f;
}

}  // namespace

TEST(GcPolicy, GreedyTakesTheEmptiestCostBenefitTheOldest) {
  Metadata m(kDataStart, 20, {}, std::make_shared<MemNodeIO>());
  m.segments[0] = {1, 300};  // old, a little fuller
  m.segments[1] = {9, 200};  // young, emptier
  m.segments[2] = {5, 0};    // empty: nothing to copy
  m.segments[3] = {6, 900};  // too full to be worth it
  m.segments[4] = {10, 10};  // the segment being written
  auto greedy = choose_victims(m, 1000, 4, 10, {.greedy = true, .max_utilization = 0.75});
  ASSERT_EQ(greedy.size(), 2u);
  EXPECT_EQ(greedy[0].index, 1u);
  EXPECT_EQ(greedy[1].index, 0u);
  auto aged = choose_victims(m, 1000, 4, 10, {.greedy = false, .max_utilization = 0.75});
  ASSERT_EQ(aged.size(), 2u);
  EXPECT_EQ(aged[0].index, 0u);  // old data tends to stay: worth more
  EXPECT_EQ(aged[0].seq, 1u);
  auto full = choose_victims(m, 1000, 4, 10, {.greedy = true, .max_utilization = 0.95});
  EXPECT_EQ(full.size(), 3u);
}

TEST(Gc, MovesLiveDataAndFreesSegments) {
  for (bool shared : {true, false}) {  // filesystems from M3 are collected too
    auto store = make_store(24 << 20, shared);
    std::mt19937_64 rng(1);
    Files files;
    {
      auto fs = Filesystem::mount(*store, options());
      files = fragment(*fs, rng, 6, 1 << 20);
      std::uint64_t free_before = fs->info().segments_free;
      std::uint64_t freed = fs->collect_garbage();
      FsInfo info = fs->info();
      EXPECT_GT(freed, 2u) << "shared records: " << shared;
      EXPECT_GT(info.segments_free, free_before);
      EXPECT_GT(info.gc.records_copied, 0u);
      EXPECT_GT(info.gc.segments_emptied, 2u);
      EXPECT_EQ(info.gc.damaged, 0u);
      files.check(*fs);
    }
    EXPECT_EQ(fsck(*store, {.repair = false, .deep = true}).exit_code(), 0);
    auto fs = Filesystem::mount(*store, options());
    files.check(*fs);
  }
}

TEST(Gc, RewritesNeverFillTheStore) {
  // Live data at 80 % of what is available, then random rewrites of ten
  // times the store's size. Without GC this fills the store within a pass.
  const std::uint64_t store_size = 16 << 20;
  auto store = make_store(store_size);
  std::mt19937_64 rng(2);
  Files files;
  std::uint64_t written = 0, live = 0;
  {
    auto fs = Filesystem::mount(*store, options());
    fs->collect_garbage();  // checkpoints: the reserve is sized from the first
    std::uint64_t available = fs->statfs().available_blocks * kBlockSize;
    const std::uint64_t file_size = 256 << 10;
    for (int i = 0; live + file_size <= available * 8 / 10; ++i) {
      InodeId ino = fs->mknod(kRootInode, "f" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
      files.write(*fs, ino, 0, noise(rng, file_size));
      live += file_size;
    }
    fs->sync();
    std::vector<InodeId> inos;
    for (const auto& [ino, d] : files.data) inos.push_back(ino);
    int n = 0;
    while (written < 10 * store_size) {
      InodeId ino = inos[rng() % inos.size()];
      std::uint64_t chunks = 1 + rng() % 4;
      std::uint64_t at = (rng() % (file_size / kChunk - chunks + 1)) * kChunk;
      try {
        files.write(*fs, ino, at, noise(rng, chunks * kChunk));
      } catch (const Error& e) {
        FsInfo info = fs->info();
        FAIL() << "after " << written << " bytes of rewrites: " << e.what() << " (segments in use "
               << info.segments_in_use << ", pending " << info.segments_pending_free << ", free "
               << info.segments_free << ", live " << info.stored_bytes << " of " << info.segment_bytes
               << " per segment; GC copied " << info.gc.records_copied << " records, emptied "
               << info.gc.segments_emptied << ")";
      }
      written += chunks * kChunk;
      if (++n % 40 == 0) fs->sync();
    }
    fs->sync();
    files.check(*fs);
    fs->collect_garbage();
    FsInfo info = fs->info();
    double used = static_cast<double>((info.segments_in_use) * info.segment_bytes);
    double amplification = static_cast<double>(info.gc.bytes_copied) / static_cast<double>(written);
    std::cout << "[ gc ] " << written / (1 << 20) << " MiB rewritten over " << live / (1 << 20)
              << " MiB live: GC copied " << info.gc.bytes_copied / (1 << 20) << " MiB (write amplification "
              << 1 + amplification << "), " << info.segments_in_use << " segments in use for "
              << static_cast<double>(info.stored_bytes) / info.segment_bytes << " segments of live data\n";
    EXPECT_GT(info.gc.records_copied, 0u);
    // What is left after GC: live data, one segment being written, and the
    // segments the newest checkpoint and log are in.
    EXPECT_LE(used, 1.3 * static_cast<double>(info.stored_bytes) + 2.0 * info.segment_bytes);
  }
  EXPECT_EQ(fsck(*store, {.repair = false, .deep = true}).exit_code(), 0);
  auto fs = Filesystem::mount(*store, options());
  files.check(*fs);
}

TEST(Gc, ChunkRewrittenDuringACopyKeepsItsNewData) {
  auto store = make_store(24 << 20);
  std::mt19937_64 rng(3);
  auto fs = Filesystem::mount(*store, options());
  Files files = fragment(*fs, rng, 6, 1 << 20);
  auto plan = FilesystemTestAccess::plan(*fs, 0.9);
  ASSERT_FALSE(plan.items.empty());
  auto records = FilesystemTestAccess::read(*fs, plan);
  // While GC holds its copies, every chunk it is about to move is rewritten.
  for (const auto& item : plan.items) {
    for (const auto& u : item.users) files.write(*fs, u.ino, u.index * kChunk, noise(rng, kChunk));
  }
  fs->sync();
  FilesystemTestAccess::apply(*fs, plan, records);
  files.check(*fs);  // nothing went back to the copied (old) data
  EXPECT_EQ(fs->info().gc.records_copied, 0u);
  fs.reset();
  EXPECT_EQ(fsck(*store, {.repair = false, .deep = true}).exit_code(), 0);
}

TEST(Gc, SegmentFreedDuringACopyIsLeftAlone) {
  auto store = make_store(24 << 20);
  std::mt19937_64 rng(4);
  auto fs = Filesystem::mount(*store, options());
  Files files = fragment(*fs, rng, 6, 1 << 20);
  auto plan = FilesystemTestAccess::plan(*fs, 0.9);
  ASSERT_FALSE(plan.items.empty());
  auto records = FilesystemTestAccess::read(*fs, plan);
  // Everything is deleted and the victims freed (and maybe reused) before
  // the copies are applied: they must be dropped, not taken for damage.
  for (const auto& [ino, d] : files.data) fs->unlink(kRootInode, "f" + std::to_string(ino - 2));
  files.data.clear();
  fs->sync();
  fs->collect_garbage();  // two checkpoints: the dead victims are free now
  EXPECT_FALSE(FilesystemTestAccess::apply(*fs, plan, records));
  FsInfo info = fs->info();
  EXPECT_EQ(info.gc.damaged, 0u);
  EXPECT_FALSE(info.errors_seen);
  fs.reset();
  EXPECT_EQ(fsck(*store, {.repair = false, .deep = true}).exit_code(), 0);
}

TEST(Gc, DamagedDataIsLeftInPlace) {
  auto store = make_store(24 << 20);
  std::mt19937_64 rng(5);
  auto fs = Filesystem::mount(*store, options());
  Files files = fragment(*fs, rng, 6, 1 << 20);
  // Damage every data record in the first segment that holds file data.
  Superblock sb = fs->superblock();
  std::size_t damaged = 0;
  for (std::uint64_t seg = 0; seg < sb.segment_count && damaged == 0; ++seg) {
    SegmentScanner scan(*store, sb, seg);
    std::vector<std::uint64_t> at;
    while (auto r = scan.next()) {
      if (r->header.type == RecordType::data_chunk) at.push_back(r->offset + r->stored_size() - 8);
    }
    for (std::uint64_t a : at) {
      Bytes b = store->read(a, 1);
      b[0] ^= 0xff;
      store->write(a, b);
      ++damaged;
    }
  }
  ASSERT_GT(damaged, 0u);
  fs->collect_garbage();
  FsInfo info = fs->info();
  EXPECT_GT(info.gc.damaged, 0u);
  EXPECT_TRUE(info.errors_seen);
  EXPECT_GT(info.gc.records_copied, 0u);  // the rest was still collected
  fs.reset();
  EXPECT_TRUE(load_superblock(*store).sb.state & state_flags::errors);
  FsckReport report = fsck(*store, {.repair = false, .deep = true});
  EXPECT_EQ(report.exit_code(), 4);
  EXPECT_FALSE(report.errors.empty());
}

TEST(Gc, RunsInTheBackgroundWhenIdle) {
  auto store = make_store(24 << 20);
  std::mt19937_64 rng(6);
  Filesystem::Options o = options();
  o.commit_interval_s = 1;
  o.gc_idle_s = 1;
  auto fs = Filesystem::mount(*store, o);
  Files files = fragment(*fs, rng, 6, 1 << 20);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (fs->info().gc.segments_emptied == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_GT(fs->info().gc.segments_emptied, 0u);
  files.check(*fs);
}
