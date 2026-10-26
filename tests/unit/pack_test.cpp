// Small files packed together: what is packed and how, changing packed
// files, dedup of small files, and GC building packs again
// (docs/M4_PLAN.md §5).

#include <algorithm>
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
#include "test_access.h"

using namespace nanoer;

namespace {

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 2;
  o.safe_space = 1 << 20;
  return o;
}

std::unique_ptr<MemStore> make_store(bool shared_records = true) {
  auto store = std::make_unique<MemStore>(48 << 20);
  make_filesystem(*store, {.segment_shift = 20, .shared_records = shared_records});  // 128 KiB chunks
  return store;
}

// Small, similar text files, like headers or configuration files: little
// to gain one by one, much together.
Bytes text(int i, std::size_t n) {
  std::string s;
  while (s.size() < n) s += "#define NANOER_OPTION_" + std::to_string(s.size() % 97) + " /* file " + std::to_string(i) + " */\n";
  s.resize(n);
  return Bytes(s.begin(), s.end());
}

std::size_t size_of(int i) { return 200 + static_cast<std::size_t>(i * 7919) % 12000; }

void expect_clean(Store& store) {
  FsckReport r = fsck(store, {.repair = false, .deep = true});
  EXPECT_EQ(r.exit_code(), 0) << (r.errors.empty() ? "" : r.errors.front());
}

Bytes read_all(Filesystem& fs, InodeId ino) {
  Bytes b(fs.getattr(ino).size);
  fs.read(ino, 0, b);
  return b;
}

const ChunkRef& ref_of(Filesystem& fs, InodeId ino) {
  return FilesystemTestAccess::metadata(fs).find(ino)->chunks.at(0);
}

}  // namespace

TEST(Pack, SmallFilesWrittenTogetherArePacked) {
  constexpr int kFiles = 200;
  std::uint64_t stored[2];
  for (bool shared : {false, true}) {  // without shared records: one by one
    auto store = make_store(shared);
    {
      auto fs = Filesystem::mount(*store, options());
      InodeId dir = fs->mkdir(kRootInode, "include", 0755, {}).ino;
      for (int i = 0; i < kFiles; ++i) {
        InodeId f = fs->mknod(dir, "h" + std::to_string(1000 + i), S_IFREG | 0644, 0, {}).ino;
        fs->write(f, 0, text(i, size_of(i)));
      }
      fs->sync();
      stored[shared] = fs->info().stored_bytes;
      if (shared) {
        std::set<std::uint64_t> packs;
        std::uint32_t last_piece = 0;
        std::uint64_t last_pack = 0;
        for (int i = 0; i < kFiles; ++i) {
          const ChunkRef& r = ref_of(*fs, fs->lookup(dir, "h" + std::to_string(1000 + i)).ino);
          EXPECT_TRUE(r.packed) << i;
          packs.insert(r.offset);
          // In name order within a pack.
          if (r.offset == last_pack) {
            EXPECT_GT(r.piece, last_piece);
          }
          last_pack = r.offset;
          last_piece = r.piece;
        }
        std::uint64_t total = 0;
        for (int i = 0; i < kFiles; ++i) total += size_of(i);
        EXPECT_LE(packs.size(), total / (64 << 10) + 3);
      }
    }
    expect_clean(*store);
    auto fs = Filesystem::mount(*store, options());
    InodeId dir = fs->lookup(kRootInode, "include").ino;
    for (int i = 0; i < kFiles; ++i) {
      ASSERT_EQ(read_all(*fs, fs->lookup(dir, "h" + std::to_string(1000 + i)).ino), text(i, size_of(i))) << i;
    }
  }
  std::cout << "[ pack ] " << kFiles << " small files: " << stored[0] << " bytes one by one, " << stored[1]
            << " packed\n";
  EXPECT_LT(stored[1] * 4, stored[0] * 3);  // /usr's small files: 30 % less (KNOWLEDGE.md §14)
}

TEST(Pack, OnlySmallFilesAndNeverAlone) {
  auto store = make_store();
  auto fs = Filesystem::mount(*store, options());
  InodeId alone = fs->mknod(kRootInode, "alone", S_IFREG | 0644, 0, {}).ino;
  fs->write(alone, 0, text(1, 3000));
  fs->sync();
  EXPECT_FALSE(ref_of(*fs, alone).packed);  // nothing to pack it with
  InodeId big = fs->mknod(kRootInode, "big", S_IFREG | 0644, 0, {}).ino;
  InodeId a = fs->mknod(kRootInode, "a", S_IFREG | 0644, 0, {}).ino;
  InodeId b = fs->mknod(kRootInode, "b", S_IFREG | 0644, 0, {}).ino;
  fs->write(big, 0, text(2, 20000));  // over 16 KiB
  fs->write(a, 0, text(3, 4000));
  fs->write(b, 0, text(4, 5000));
  fs->sync();
  EXPECT_FALSE(ref_of(*fs, big).packed);
  EXPECT_TRUE(ref_of(*fs, a).packed);
  EXPECT_EQ(ref_of(*fs, a).offset, ref_of(*fs, b).offset);
}

TEST(Pack, ChangingPackedFiles) {
  auto store = make_store();
  std::map<std::string, Bytes> want;
  {
    auto fs = Filesystem::mount(*store, options());
    for (int i = 0; i < 20; ++i) {
      std::string name = "f" + std::to_string(i);
      want[name] = text(i, size_of(i));
      fs->write(fs->mknod(kRootInode, name, S_IFREG | 0644, 0, {}).ino, 0, want[name]);
    }
    fs->sync();
    auto ino = [&](const std::string& n) { return fs->lookup(kRootInode, n).ino; };
    // Overwrite the middle of one, append to another (past 16 KiB: no
    // longer packed), truncate a third, punch a hole, rename, delete.
    fs->write(ino("f1"), 50, text(99, 40));
    std::copy_n(text(99, 40).begin(), 40, want["f1"].begin() + 50);
    Bytes more = text(98, 20000);
    fs->write(ino("f2"), want["f2"].size(), more);
    want["f2"].insert(want["f2"].end(), more.begin(), more.end());
    fs->setattr(ino("f3"), {.size = 77});
    want["f3"].resize(77);
    fs->rename(kRootInode, "f4", kRootInode, "renamed", 0);
    want["renamed"] = want["f4"];
    want.erase("f4");
    fs->unlink(kRootInode, "f5");
    want.erase("f5");
    fs->sync();
    EXPECT_FALSE(ref_of(*fs, ino("f2")).packed);
    for (const auto& [n, d] : want) EXPECT_EQ(read_all(*fs, ino(n)), d) << n;
  }
  expect_clean(*store);
  {
    auto fs = Filesystem::mount(*store, options());
    for (const auto& [n, d] : want) EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, n).ino), d) << n;
    for (const auto& [n, d] : want) fs->unlink(kRootInode, n);
    fs->sync();
    EXPECT_EQ(fs->info().stored_bytes, 0u);  // every pack released with its last piece
  }
  expect_clean(*store);
}

TEST(Pack, IdenticalSmallFilesShareAPiece) {
  auto store = make_store();
  auto fs = Filesystem::mount(*store, options());
  for (int i = 0; i < 10; ++i) {
    fs->write(fs->mknod(kRootInode, "f" + std::to_string(i), S_IFREG | 0644, 0, {}).ino, 0, text(i, 3000));
  }
  fs->sync();
  std::uint64_t stored = fs->info().stored_bytes;
  // The same contents again, later: each takes the existing piece.
  for (int i = 0; i < 10; ++i) {
    fs->write(fs->mknod(kRootInode, "copy" + std::to_string(i), S_IFREG | 0644, 0, {}).ino, 0, text(i, 3000));
  }
  fs->sync();
  EXPECT_EQ(fs->info().stored_bytes, stored);
  EXPECT_EQ(fs->info().dedup_hits, 10u);
  for (int i = 0; i < 10; ++i) {
    InodeId a = fs->lookup(kRootInode, "f" + std::to_string(i)).ino;
    InodeId b = fs->lookup(kRootInode, "copy" + std::to_string(i)).ino;
    EXPECT_EQ(ref_of(*fs, a), ref_of(*fs, b));
    EXPECT_EQ(read_all(*fs, b), text(i, 3000));
  }
  fs.reset();
  expect_clean(*store);
}

TEST(Pack, GarbageCollectionPacksTheLivePiecesAgain) {
  auto store = make_store();
  std::map<std::string, Bytes> want;
  {
    auto fs = Filesystem::mount(*store, options());
    for (int i = 0; i < 300; ++i) {
      std::string name = "f" + std::to_string(i);
      want[name] = text(i, size_of(i));
      fs->write(fs->mknod(kRootInode, name, S_IFREG | 0644, 0, {}).ino, 0, want[name]);
    }
    fs->sync();
    // Delete three files in four: the packs are mostly dead.
    for (int i = 0; i < 300; ++i) {
      if (i % 4 == 0) continue;
      std::string name = "f" + std::to_string(i);
      fs->unlink(kRootInode, name);
      want.erase(name);
    }
    // More data after them, so the packs' segment is no longer the one
    // being written (GC leaves that one alone).
    Bytes filler(1536 << 10);
    std::mt19937_64 rng(1);
    for (auto& b : filler) b = static_cast<std::uint8_t>(rng());  // incompressible
    fs->write(fs->mknod(kRootInode, "filler", S_IFREG | 0644, 0, {}).ino, 0, filler);
    fs->sync();
    std::uint64_t packed_before = 0;
    {
      std::set<std::uint64_t> seen;
      for (const auto& [n, d] : want) {
        const ChunkRef& r = ref_of(*fs, fs->lookup(kRootInode, n).ino);
        if (r.packed && seen.insert(r.offset).second) packed_before += r.stored_size;
      }
    }
    fs->collect_garbage(0.97);
    FsInfo info = fs->info();
    EXPECT_GT(info.gc.packs_repacked, 0u);
    // The packs' space shrank to about the quarter that is still live.
    std::set<std::uint64_t> seen;
    std::uint64_t unique_after = 0;
    for (const auto& [n, d] : want) {
      const ChunkRef& r = ref_of(*fs, fs->lookup(kRootInode, n).ino);
      if (r.packed && seen.insert(r.offset).second) unique_after += r.stored_size;
    }
    EXPECT_LT(unique_after * 2, packed_before);
    for (const auto& [n, d] : want) EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, n).ino), d) << n;
  }
  expect_clean(*store);
  auto fs = Filesystem::mount(*store, options());
  for (const auto& [n, d] : want) EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, n).ino), d) << n;
}
