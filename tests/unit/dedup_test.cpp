// Dedup: the index, identical data stored once, every change to one copy
// leaving the others alone, and dedup with GC (docs/M4_PLAN.md §4).

#include <algorithm>
#include <random>
#include <unordered_map>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/checksum.h"
#include "nanoer/dedup.h"
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

constexpr std::uint64_t kChunk = 16 << 10;

std::unique_ptr<MemStore> make_store(bool dedup = true) {
  auto store = std::make_unique<MemStore>(48 << 20);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14, .dedup = dedup});
  return store;
}

Bytes noise(std::uint64_t seed, std::size_t n) {
  std::mt19937_64 rng(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

InodeId create(Filesystem& fs, const std::string& name, ByteView data) {
  InodeId ino = fs.mknod(kRootInode, name, S_IFREG | 0644, 0, {}).ino;
  fs.write(ino, 0, data);
  return ino;
}

Bytes read_all(Filesystem& fs, InodeId ino) {
  Bytes b(fs.getattr(ino).size);
  fs.read(ino, 0, b);
  return b;
}

void expect_clean(Store& store) {
  FsckReport r = fsck(store, {.repair = false, .deep = true});
  EXPECT_EQ(r.exit_code(), 0) << (r.errors.empty() ? "" : r.errors.front());
}

}  // namespace

TEST(DedupIndex, MatchesAMapThroughInsertsAndErasures) {
  DedupIndex index;
  std::unordered_map<std::uint64_t, DedupTarget> model;
  std::mt19937_64 rng(1);
  std::vector<std::uint64_t> keys;
  for (int i = 0; i < 20000; ++i) keys.push_back(rng() | 1);
  for (int round = 0; round < 60000; ++round) {
    std::uint64_t key = keys[rng() % keys.size()];
    DedupTarget t{(rng() % (1ull << 40)) * 8, false, 0};
    if (rng() % 3 == 0) t = {t.offset, true, static_cast<std::uint32_t>(rng() % 65536)};
    if (rng() % 2) {
      index.put(key, t);
      model[key] = t;
    } else if (auto it = model.find(key); it != model.end()) {
      index.erase(key, rng() % 4 ? it->second : t);  // a stale target leaves it
      if (index.find(key) != std::optional<DedupTarget>(it->second)) model.erase(it);
    }
  }
  EXPECT_EQ(index.size(), model.size());
  for (std::uint64_t key : keys) {
    auto it = model.find(key);
    EXPECT_EQ(index.find(key), it == model.end() ? std::nullopt : std::optional<DedupTarget>(it->second));
  }
  EXPECT_LE(index.memory_bytes(), 16 * 2 * std::max<std::size_t>(1024, model.size() * 4 / 3) + 16);
}

TEST(DedupIndex, TargetsItCannotHoldAreLeftOut) {
  DedupIndex index;
  index.put(5, {1ull << 50, false, 0});  // beyond 256 TiB
  index.put(6, {64, true, 70000});       // a piece past 64 KiB
  EXPECT_EQ(index.size(), 0u);
  EXPECT_FALSE(index.find(5));
  index.erase(6, {64, true, 70000});  // and erasing one is harmless
}

TEST(Dedup, IdenticalFilesAreStoredOnce) {
  auto store = make_store();
  Bytes data = noise(1, 20 * kChunk + 1000);  // the last chunk is partial
  {
    auto fs = Filesystem::mount(*store, options());
    create(*fs, "a", data);
    fs->sync();
    std::uint64_t one = fs->info().stored_bytes;
    create(*fs, "b", data);  // a later commit
    create(*fs, "c", data);  // and twice within one commit
    create(*fs, "d", data);
    fs->sync();
    FsInfo info = fs->info();
    EXPECT_TRUE(info.dedup);
    EXPECT_EQ(info.stored_bytes, one);
    EXPECT_EQ(info.dedup_hits, 3 * 21u);
    for (const char* n : {"a", "b", "c", "d"}) EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, n).ino), data);
  }
  expect_clean(*store);
  auto fs = Filesystem::mount(*store, options());
  for (const char* n : {"a", "b", "c", "d"}) EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, n).ino), data);
  EXPECT_EQ(fs->info().dedup_entries, 0u) << "rebuilt after mount";
  fs->rebuild_dedup();
  EXPECT_EQ(fs->info().dedup_entries, 21u);
}

TEST(Dedup, ChangingOneCopyLeavesTheOthers) {
  auto store = make_store();
  Bytes data = noise(2, 16 * kChunk);
  {
    auto fs = Filesystem::mount(*store, options());
    InodeId a = create(*fs, "a", data);
    fs->sync();
    InodeId b = create(*fs, "b", data);
    InodeId c = create(*fs, "c", data);
    fs->sync();
    // Overwrite part of b, truncate c, delete a.
    Bytes patch = noise(3, kChunk);
    fs->write(b, 3 * kChunk, patch);
    fs->setattr(c, {.size = 5 * kChunk + 7});
    fs->unlink(kRootInode, "a");
    fs->sync();
    Bytes want_b = data;
    std::copy(patch.begin(), patch.end(), want_b.begin() + 3 * kChunk);
    EXPECT_EQ(read_all(*fs, b), want_b);
    EXPECT_EQ(read_all(*fs, c), Bytes(data.begin(), data.begin() + 5 * kChunk + 7));
    (void)a;
  }
  expect_clean(*store);
  {
    auto fs = Filesystem::mount(*store, options());
    fs->unlink(kRootInode, "b");
    fs->unlink(kRootInode, "c");
    fs->sync();
    EXPECT_EQ(fs->info().stored_bytes, 0u);  // the last reference released it all
    EXPECT_EQ(fs->info().dedup_entries, 0u);
  }
  expect_clean(*store);
}

TEST(Dedup, OldContentMovesToAnotherFileInTheSameCommit) {
  // A file's chunk is rewritten while another file takes its old content,
  // in one commit: the record must not be released while the other refers
  // to it. Chunks are committed in no fixed order, so both orders are made.
  auto store = make_store();
  struct Pair {
    std::string from, to;
    Bytes old_data, new_data;
  };
  std::vector<Pair> pairs = {{"a", "b", noise(4, 4 * kChunk), noise(5, 4 * kChunk)},
                             {"c", "d", noise(14, 4 * kChunk), noise(15, 4 * kChunk)}};
  {
    auto fs = Filesystem::mount(*store, options());
    for (const Pair& p : pairs) create(*fs, p.from, p.old_data);
    fs->sync();
    // First pair: rewrite, then copy; second pair: copy, then rewrite.
    fs->write(fs->lookup(kRootInode, "a").ino, 0, pairs[0].new_data);
    create(*fs, "b", pairs[0].old_data);
    create(*fs, "d", pairs[1].old_data);
    fs->write(fs->lookup(kRootInode, "c").ino, 0, pairs[1].new_data);
    fs->sync();
    for (const Pair& p : pairs) {
      EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, p.from).ino), p.new_data);
      EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, p.to).ino), p.old_data);
    }
  }
  expect_clean(*store);
  auto fs = Filesystem::mount(*store, options());
  for (const Pair& p : pairs) EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, p.to).ino), p.old_data);
}

TEST(Dedup, AWrongIndexEntryIsNeverTrusted) {
  // As if two contents had the same 8-byte key: the full BLAKE3 decides.
  auto store = make_store();
  Bytes a_data = noise(6, kChunk), b_data = noise(7, kChunk);
  auto fs = Filesystem::mount(*store, options());
  InodeId a = create(*fs, "a", a_data);
  fs->sync();
  ChunkRef a_ref = FilesystemTestAccess::chunks(*fs, a).at(0);
  Blake3Hash b_id = blake3(b_data);
  FilesystemTestAccess::set_index(*fs, hash_key(b_id.data()), {a_ref.offset, false, 0});
  InodeId b = create(*fs, "b", b_data);
  fs->sync();
  EXPECT_EQ(fs->info().dedup_hits, 0u);
  EXPECT_EQ(read_all(*fs, b), b_data);
  EXPECT_EQ(read_all(*fs, a), a_data);
  fs.reset();
  expect_clean(*store);
}

TEST(Dedup, CanBeTurnedOff) {
  Bytes data = noise(8, 8 * kChunk);
  for (int way = 0; way < 2; ++way) {
    auto store = make_store(way != 0);  // mkfs --no-dedup, or -o nodedup
    Filesystem::Options o = options();
    o.dedup = way != 0 ? false : true;
    auto fs = Filesystem::mount(*store, o);
    create(*fs, "a", data);
    create(*fs, "b", data);
    fs->sync();
    FsInfo info = fs->info();
    EXPECT_FALSE(info.dedup);
    EXPECT_EQ(info.dedup_hits, 0u);
    EXPECT_EQ(info.dedup_entries, 0u);  // records without an identity
    fs.reset();
    expect_clean(*store);
  }
}

TEST(Dedup, GarbageCollectionMovesSharedRecords) {
  auto store = make_store();
  Bytes shared = noise(9, 32 * kChunk);
  {
    auto fs = Filesystem::mount(*store, options());
    create(*fs, "a", shared);
    create(*fs, "b", shared);
    fs->sync();
    // Other data after it in the same segments, mostly rewritten later: the
    // segment holding the shared records becomes worth collecting.
    std::vector<InodeId> others;
    for (int i = 0; i < 8; ++i) others.push_back(create(*fs, "o" + std::to_string(i), noise(100 + i, 32 * kChunk)));
    fs->sync();
    for (InodeId o : others) fs->write(o, 0, noise(200 + o, 28 * kChunk));
    fs->sync();
    InodeId a = fs->lookup(kRootInode, "a").ino, b = fs->lookup(kRootInode, "b").ino;
    auto chunks = [&](InodeId ino) { return FilesystemTestAccess::chunks(*fs, ino); };
    auto placed = chunks(a);
    ASSERT_EQ(placed, chunks(b));
    fs->collect_garbage(0.97);
    FsInfo info = fs->info();
    EXPECT_GT(info.gc.records_copied, 0u);
    std::size_t moved = 0;
    for (const auto& [index, ref] : chunks(a)) moved += ref.offset != placed.at(index).offset;
    EXPECT_GT(moved, 0u) << "GC should have moved shared records";
    EXPECT_EQ(chunks(a), chunks(b));  // still shared, at the new places
    EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, "a").ino), shared);
    EXPECT_EQ(read_all(*fs, fs->lookup(kRootInode, "b").ino), shared);
    // The index follows the copies: a new copy still deduplicates.
    std::uint64_t before = fs->info().stored_bytes;
    create(*fs, "c", shared);
    fs->sync();
    EXPECT_EQ(fs->info().stored_bytes, before);
  }
  expect_clean(*store);
}
