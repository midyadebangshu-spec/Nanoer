// Damage (bit rot, overwritten blocks): never a crash, never wrong data,
// never a silently shortened log.

#include <algorithm>
#include <functional>
#include <random>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"
#include "nanoer/segment.h"

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

struct Found {
  std::uint64_t offset;
  RecordType type;
  std::uint64_t size;
};

// Every intact record in the store, in segment order.
std::vector<Found> records(Store& store) {
  Superblock sb = load_superblock(store).sb;
  std::vector<Found> out;
  for (std::uint64_t i = 0; i < sb.segment_count; ++i) {
    SegmentScanner scan(store, sb, i);
    while (auto r = scan.next()) out.push_back({r->offset, r->header.type, r->stored_size()});
  }
  return out;
}

void flip(MemStore& store, std::uint64_t offset) {
  Bytes b = store.read(offset, 1);
  b[0] ^= 0xff;
  store.write(offset, b);
}

Bytes data_for(int i, std::size_t n = 30000) {
  Bytes d(n);
  for (std::size_t j = 0; j < n; ++j) d[j] = static_cast<std::uint8_t>((j * 7 + static_cast<std::size_t>(i)) % 251);
  return d;
}

// A filesystem with ten files, each written and synced in its own commit.
std::unique_ptr<MemStore> ten_commits() {
  auto store = std::make_unique<MemStore>(32 << 20);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14});
  auto fs = Filesystem::mount(*store, options());
  for (int i = 0; i < 10; ++i) {
    InodeId f = fs->mknod(kRootInode, "f" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
    fs->write(f, 0, data_for(i));
    fs->sync();
  }
  fs->abandon();  // as after a crash: the log after the checkpoint matters
  return store;
}

int count_files(Filesystem& fs) { return static_cast<int>(fs.list(kRootInode).size()) - 2; }

}  // namespace

TEST(Damage, MidLogDamageIsDetectedNotSilentlyDropped) {
  auto store = ten_commits();
  std::vector<Found> txns;
  for (const Found& r : records(*store)) {
    if (r.type == RecordType::metadata_log) txns.push_back(r);
  }
  ASSERT_GE(txns.size(), 10u);
  // Damage the 4th transaction of the ten.
  const Found& victim = txns[txns.size() - 7];
  flip(*store, victim.offset + victim.size / 2);

  auto fs = Filesystem::mount(*store, options());
  FsInfo info = fs->info();
  EXPECT_TRUE(info.log_damaged) << "damage taken for the end of the log";
  EXPECT_TRUE(fs->read_only()) << "must not write over the rest of the log";
  EXPECT_EQ(count_files(*fs), 3) << "the state up to the damage";
  int err = 0;
  try {
    fs->mknod(kRootInode, "new", S_IFREG | 0644, 0, {});
  } catch (const Error& e) {
    err = e.to_errno();
  }
  EXPECT_EQ(err, EROFS);
}

TEST(Damage, DamagedLastTransactionAloneLooksLikeACrash) {
  // A known limit: damage to the very last transaction, with nothing after
  // it, cannot be told apart from a write cut short by a power cut.
  auto store = ten_commits();
  std::vector<Found> all = records(*store);
  auto last = std::find_if(all.rbegin(), all.rend(),
                           [](const Found& r) { return r.type == RecordType::metadata_log; });
  ASSERT_NE(last, all.rend());
  flip(*store, last->offset + last->size / 2);
  auto fs = Filesystem::mount(*store, options());
  EXPECT_FALSE(fs->info().log_damaged);
  EXPECT_EQ(count_files(*fs), 9);
}

TEST(Damage, DamagedNewestCheckpointFallsBackToThePreviousOne) {
  auto store = std::make_unique<MemStore>(32 << 20);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14});
  for (int round = 0; round < 2; ++round) {
    auto fs = Filesystem::mount(*store, options());
    for (int i = 0; i < 3; ++i) {
      InodeId f = fs->mknod(kRootInode, "r" + std::to_string(round) + "-" + std::to_string(i),
                            S_IFREG | 0644, 0, {}).ino;
      fs->write(f, 0, data_for(i));
      fs->sync();
    }
    // Unmounting writes a checkpoint.
  }
  Superblock sb = load_superblock(*store).sb;
  flip(*store, sb.checkpoint_offset + 40);  // inside the newest checkpoint's first part

  auto fs = Filesystem::mount(*store, options());
  FsInfo info = fs->info();
  EXPECT_TRUE(info.checkpoint_fallback);
  EXPECT_FALSE(info.log_damaged);
  EXPECT_FALSE(fs->read_only()) << "nothing was lost, so it stays writable";
  EXPECT_EQ(count_files(*fs), 6) << "all six files, rebuilt from the previous checkpoint and the log";
  Bytes back(30000);
  fs->read(fs->lookup(kRootInode, "r1-2").ino, 0, back);
  EXPECT_EQ(back, data_for(2));
  fs.reset();  // unmount: the errors flag is recorded
  auto again = Filesystem::mount(*store, options());
  EXPECT_TRUE(again->info().errors_flag);
  EXPECT_FALSE(again->info().checkpoint_fallback) << "the unmount wrote a good checkpoint";
}

TEST(Damage, DamagedFileDataIsAnErrorNeverWrongData) {
  auto store = std::make_unique<MemStore>(32 << 20);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14});
  {
    auto fs = Filesystem::mount(*store, options());
    InodeId f = fs->mknod(kRootInode, "f", S_IFREG | 0644, 0, {}).ino;
    fs->write(f, 0, data_for(1, 100000));
  }
  std::vector<Found> chunks;
  for (const Found& r : records(*store)) {
    if (r.type == RecordType::data_chunk) chunks.push_back(r);
  }
  ASSERT_GE(chunks.size(), 6u);
  flip(*store, chunks[2].offset + chunks[2].size - 20);  // inside chunk 2's payload

  auto fs = Filesystem::mount(*store, options());
  InodeId f = fs->lookup(kRootInode, "f").ino;
  Bytes buf(16384);
  EXPECT_EQ(fs->read(f, 0, buf), buf.size());  // chunk 0 is fine
  int err = 0;
  try {
    fs->read(f, 2 * 16384 + 100, buf);
  } catch (const Error& e) {
    err = e.to_errno();
  }
  EXPECT_EQ(err, EUCLEAN);
  EXPECT_EQ(fs->info().read_errors, 1u);
  fs.reset();
  EXPECT_TRUE(Filesystem::mount(*store, options())->info().errors_flag);
}

TEST(Damage, RandomDamageNeverCrashes) {
  auto pristine = ten_commits();
  Bytes image = pristine->image();
  std::vector<Found> all = records(*pristine);
  std::mt19937_64 rng(5);
  for (int trial = 0; trial < 300; ++trial) {
    MemStore store(image);
    int hits = 1 + static_cast<int>(rng() % 4);
    for (int h = 0; h < hits; ++h) {
      // Mostly inside records (where it matters), sometimes anywhere.
      std::uint64_t at = rng() % 3 ? all[rng() % all.size()].offset + rng() % 64
                                   : rng() % (4 << 20);
      flip(store, at);
    }
    try {
      auto fs = Filesystem::mount(store, options());
      for (const DirEntry& e : fs->list(kRootInode)) {
        if (e.name == "." || e.name == "..") continue;
        Bytes buf(30000);
        try {
          fs->read(e.ino, 0, buf);
        } catch (const Error& err) {
          ASSERT_EQ(err.code(), Errc::corrupt) << err.what();
        }
      }
      fs->abandon();
    } catch (const Error& e) {
      // Refusing to mount is fine; crashing or returning wrong data is not.
      EXPECT_TRUE(e.code() == Errc::corrupt || e.code() == Errc::unsupported) << e.what();
    }
  }
}
