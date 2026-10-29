// Cold data recompressed: what becomes cold and in which form, files that
// change or go away while their data is recompressed, shared and packed
// data, GC recompressing what it would copy, damage, space, and the
// background pass (docs/M5_PLAN.md §3).

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/legacy_metadata.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"
#include "nanoer/segment.h"
#include "test_access.h"

using namespace nanoer;

namespace {

constexpr std::uint64_t kChunk = 128 << 10;

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 2;
  o.safe_space = 16 << 20;
  return o;
}

// 4 MiB segments and 128 KiB chunks.
std::unique_ptr<MemStore> make_store(std::uint64_t size = 96 << 20, bool cold_data = true) {
  auto store = std::make_unique<MemStore>(size);
  make_filesystem(*store, {.segment_shift = 22, .cold_data = cold_data});
  return store;
}

// Log lines: different in every chunk, and much alike across chunks, which
// is what a higher level gains from.
Bytes log_text(std::mt19937_64& rng, std::size_t n) {
  static const char* const words[] = {"connection", "accepted", "from", "user", "session", "opened",
                                      "closed", "timeout", "request", "GET", "POST", "/api/v1/items",
                                      "status", "200", "404", "latency", "ms", "worker", "cache", "miss"};
  std::string s;
  while (s.size() < n) {
    s += "2026-10-27T12:" + std::to_string(rng() % 60) + ":" + std::to_string(rng() % 60) + " INFO";
    for (int w = 0; w < 8; ++w) s += std::string(" ") + words[rng() % 20];
    s += " id=" + std::to_string(rng() % 100000) + "\n";
  }
  s.resize(n);
  return Bytes(s.begin(), s.end());
}

Bytes noise(std::mt19937_64& rng, std::size_t n) {
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

struct Files {
  std::map<InodeId, Bytes> data;
  std::map<InodeId, std::string> names;

  InodeId create(Filesystem& fs, const std::string& name, const Bytes& bytes) {
    InodeId ino = fs.mknod(kRootInode, name, S_IFREG | 0644, 0, {}).ino;
    if (!bytes.empty()) fs.write(ino, 0, bytes);
    data[ino] = bytes;
    names[ino] = name;
    return ino;
  }
  void write(Filesystem& fs, InodeId ino, std::uint64_t at, ByteView bytes) {
    fs.write(ino, at, bytes);
    Bytes& d = data[ino];
    if (d.size() < at + bytes.size()) d.resize(at + bytes.size());
    std::copy(bytes.begin(), bytes.end(), d.begin() + static_cast<std::ptrdiff_t>(at));
  }
  void remove(Filesystem& fs, const std::string& name) {
    for (auto it = names.begin(); it != names.end(); ++it) {
      if (it->second != name) continue;
      fs.unlink(kRootInode, name);
      data.erase(it->first);
      names.erase(it);
      return;
    }
  }
  void check(Filesystem& fs) const {
    for (const auto& [ino, expected] : data) {
      Bytes got(expected.size());
      ASSERT_EQ(fs.getattr(ino).size, expected.size()) << "inode " << ino;
      ASSERT_EQ(fs.read(ino, 0, got), got.size()) << "inode " << ino;
      ASSERT_TRUE(got == expected) << "contents of inode " << ino;
    }
  }
};

std::map<std::uint64_t, ChunkRef> chunks(Filesystem& fs, InodeId ino) { return FilesystemTestAccess::chunks(fs, ino); }

std::size_t cold_chunks(Filesystem& fs, InodeId ino) {
  std::size_t n = 0;
  for (const auto& [index, ref] : chunks(fs, ino)) n += ref.cold;
  return n;
}

void expect_clean(Store& store) {
  FsckReport r = fsck(store, {.repair = false, .deep = true});
  EXPECT_EQ(r.exit_code(), 0) << (r.errors.empty() ? "" : r.errors.front());
}

std::uint64_t stored(Filesystem& fs) { return fs.info().stored_bytes; }

}  // namespace

TEST(Tier, ColdChunksAndPacksAreRecompressed) {
  auto store = make_store();
  std::mt19937_64 rng(1);
  Files files;
  InodeId big, lone;
  std::vector<InodeId> small;
  {
    auto fs = Filesystem::mount(*store, options());
    big = files.create(*fs, "big", log_text(rng, 20 * kChunk));
    lone = files.create(*fs, "lone", log_text(rng, 100 << 10));
    for (int i = 0; i < 6; ++i) small.push_back(files.create(*fs, "s" + std::to_string(i), log_text(rng, 4000)));
    fs->sync();
    ASSERT_TRUE(chunks(*fs, small[0]).at(0).packed);
    std::uint64_t before = stored(*fs);

    EXPECT_EQ(fs->recompress_cold(3600), 0u);  // written just now: not cold after an hour
    EXPECT_EQ(fs->recompress_cold(0), 20u + 1 + 6);
    FsInfo info = fs->info();
    // Every chunk on its own (each keeps its identity), and the pack again.
    EXPECT_EQ(info.tier.singles, 20u + 1);
    EXPECT_EQ(info.tier.packs, 1u);
    EXPECT_EQ(info.tier.in_place, 0u);
    for (const auto& [index, ref] : chunks(*fs, big)) EXPECT_TRUE(ref.cold && !ref.packed && ref.hash != 0);
    const ChunkRef l = chunks(*fs, lone).at(0);
    EXPECT_TRUE(l.cold && !l.packed && l.hash != 0);
    for (InodeId s : small) EXPECT_TRUE(chunks(*fs, s).at(0).cold);
    EXPECT_LT(stored(*fs), before * 97 / 100) << "zstd 9: smaller";
    files.check(*fs);
    EXPECT_EQ(fs->recompress_cold(0), 0u);  // nothing is examined twice
  }
  expect_clean(*store);
  auto fs = Filesystem::mount(*store, options());
  files.check(*fs);
  EXPECT_EQ(cold_chunks(*fs, big), 20u);
}

TEST(Tier, IncompressibleDataIsOnlyMarked) {
  auto store = make_store();
  std::mt19937_64 rng(2);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  InodeId ino = files.create(*fs, "random", noise(rng, 10 * kChunk));
  fs->sync();
  std::map<std::uint64_t, ChunkRef> before = chunks(*fs, ino);
  EXPECT_EQ(fs->recompress_cold(0), 10u);
  FsInfo info = fs->info();
  EXPECT_EQ(info.tier.singles, 0u);
  EXPECT_EQ(info.tier.in_place, 10u);  // not rewritten
  for (const auto& [index, ref] : chunks(*fs, ino)) {
    EXPECT_TRUE(ref.cold);
    EXPECT_EQ(ref.offset, before.at(index).offset);
  }
  files.check(*fs);
  fs.reset();
  expect_clean(*store);
}

TEST(Tier, ChunkWrittenDuringRecompressionKeepsItsNewData) {
  auto store = make_store();
  std::mt19937_64 rng(3);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  InodeId ino = files.create(*fs, "f", log_text(rng, 16 * kChunk));
  fs->sync();
  auto plan = FilesystemTestAccess::tier_plan(*fs, 0);
  ASSERT_EQ(plan.items.size(), 16u);
  auto reads = FilesystemTestAccess::tier_read(*fs, plan);
  // While they are being recompressed, chunks 3 and 12 change.
  files.write(*fs, ino, 3 * kChunk, log_text(rng, kChunk));
  files.write(*fs, ino, 12 * kChunk + 100, log_text(rng, 50));
  fs->sync();
  EXPECT_EQ(FilesystemTestAccess::tier_apply(*fs, plan, reads), 14u);
  EXPECT_FALSE(chunks(*fs, ino).at(3).cold);
  EXPECT_FALSE(chunks(*fs, ino).at(12).cold);
  files.check(*fs);  // nothing went back to the data that was read
  // The next pass takes them.
  EXPECT_EQ(fs->recompress_cold(0), 2u);
  files.check(*fs);
  fs.reset();
  expect_clean(*store);
}

TEST(Tier, FileDeletedDuringRecompressionIsDropped) {
  auto store = make_store();
  std::mt19937_64 rng(4);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  files.create(*fs, "gone", log_text(rng, 8 * kChunk));
  InodeId kept = files.create(*fs, "kept", log_text(rng, 8 * kChunk));
  fs->sync();
  auto plan = FilesystemTestAccess::tier_plan(*fs, 0);
  auto reads = FilesystemTestAccess::tier_read(*fs, plan);
  fs->unlink(kRootInode, "gone");
  files.data.erase(kept - 1);
  fs->sync();
  fs->collect_garbage();  // and its segments freed, maybe reused
  EXPECT_EQ(FilesystemTestAccess::tier_apply(*fs, plan, reads), 8u);
  EXPECT_EQ(cold_chunks(*fs, kept), 8u);
  FsInfo info = fs->info();
  EXPECT_EQ(info.tier.damaged, 0u);
  EXPECT_FALSE(info.errors_seen);
  files.check(*fs);
  fs.reset();
  expect_clean(*store);
}

TEST(Tier, SharedDataIsRecompressedOnceForAllItsUsers) {
  auto store = make_store();
  std::mt19937_64 rng(5);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  Bytes same = log_text(rng, 3 * kChunk);
  InodeId a = files.create(*fs, "a", same);
  InodeId b = files.create(*fs, "b", same);
  fs->sync();
  ASSERT_EQ(chunks(*fs, a).at(1).offset, chunks(*fs, b).at(1).offset);
  // While one user is still being written, its shared data is left alone.
  Files more;
  InodeId c = more.create(*fs, "c", same);
  fs->sync();
  more.write(*fs, c, 5 * kChunk, log_text(rng, 10));  // not committed: c is not cold
  EXPECT_TRUE(FilesystemTestAccess::tier_plan(*fs, 0).items.empty());
  fs->sync();
  EXPECT_EQ(fs->recompress_cold(0), 9u + 1);  // and c's new chunk alone
  const Metadata& m = FilesystemTestAccess::metadata(*fs);
  for (std::uint64_t i = 0; i < 3; ++i) {
    const ChunkRef r = *m.chunk(a, i);
    EXPECT_TRUE(r.cold && !r.packed);
    EXPECT_EQ(r, *m.chunk(b, i));
    EXPECT_EQ(r, *m.chunk(c, i));
    EXPECT_EQ(m.references(r), 3u);
  }
  EXPECT_EQ(fs->info().tier.singles + fs->info().tier.in_place, 3u + 1);
  files.check(*fs);
  more.check(*fs);
  fs.reset();
  expect_clean(*store);
}

TEST(Tier, NewCopiesOfColdDataAreStillDeduplicated) {
  auto store = make_store();
  std::mt19937_64 rng(6);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  Bytes data = log_text(rng, 8 * kChunk);
  files.create(*fs, "original", data);
  fs->sync();
  ASSERT_EQ(fs->recompress_cold(0), 8u);
  std::uint64_t before = stored(*fs);
  // Cold records keep their identity: still in the dedup index.
  files.create(*fs, "copy", data);
  fs->sync();
  EXPECT_EQ(fs->info().dedup_hits, 8u);
  EXPECT_LT(stored(*fs), before + 1024);
  files.check(*fs);
  fs.reset();
  expect_clean(*store);
}

TEST(Tier, GarbageCollectionRecompressesColdDataInsteadOfCopyingIt) {
  auto store = make_store();
  std::mt19937_64 rng(7);
  Files files;
  Filesystem::Options o = options();
  o.cold_after_s = 0;
  auto fs = Filesystem::mount(*store, o);
  for (int i = 0; i < 6; ++i) files.create(*fs, "f" + std::to_string(i), log_text(rng, 24 * kChunk));
  fs->sync();
  // Half of every file rewritten: the first segments are half dead.
  for (auto& [ino, d] : std::map<InodeId, Bytes>(files.data)) {
    for (std::uint64_t at = 0; at < d.size(); at += 2 * kChunk) files.write(*fs, ino, at, log_text(rng, kChunk));
  }
  fs->sync();
  fs->collect_garbage();
  FsInfo info = fs->info();
  EXPECT_GT(info.tier.singles, 0u);
  EXPECT_GT(info.gc.segments_emptied, 0u);
  // The cold files' data was recompressed, not copied first: GC copied only
  // some of what is cold already.
  for (const auto& [ino, d] : files.data) EXPECT_EQ(cold_chunks(*fs, ino), 24u);
  EXPECT_LT(info.gc.bytes_copied, info.tier.stored_after);
  files.check(*fs);
  fs.reset();
  expect_clean(*store);
}

TEST(Tier, DamagedDataIsLeftInPlace) {
  auto store = make_store();
  std::mt19937_64 rng(8);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  InodeId ino = files.create(*fs, "f", log_text(rng, 16 * kChunk));
  fs->sync();
  // Damage the record of chunk 2.
  const ChunkRef r = chunks(*fs, ino).at(2);
  Bytes b = store->read(r.offset + r.stored_size - 8, 1);
  b[0] ^= 0xff;
  store->write(r.offset + r.stored_size - 8, b);
  EXPECT_EQ(fs->recompress_cold(0), 15u);  // the other chunks
  FsInfo info = fs->info();
  EXPECT_EQ(info.tier.damaged, 1u);
  EXPECT_TRUE(info.errors_seen);
  EXPECT_EQ(chunks(*fs, ino).at(2), r);
  EXPECT_EQ(fs->recompress_cold(0), 0u);  // not tried again
  Bytes out(kChunk);
  EXPECT_THROW(fs->read(ino, 2 * kChunk, out), Error);
  fs.reset();
  EXPECT_EQ(fsck(*store, {.repair = false, .deep = true}).exit_code(), 4);
}

TEST(Tier, NothingWhenSpaceIsShort) {
  auto store = make_store(48 << 20);
  std::mt19937_64 rng(9);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  fs->collect_garbage();
  // Incompressible data nearly fills the filesystem; then a cold file.
  std::uint64_t available = fs->statfs().available_blocks * kBlockSize;
  InodeId fill = files.create(*fs, "fill", {});
  for (std::uint64_t at = 0; at + (1 << 20) < available * 9 / 10; at += 1 << 20) {
    files.write(*fs, fill, at, noise(rng, 1 << 20));
    fs->sync();
  }
  files.create(*fs, "cold", log_text(rng, 3 * kChunk));
  fs->sync();
  EXPECT_EQ(fs->recompress_cold(0), 0u);  // no room: nothing written, no error
  files.check(*fs);
  files.write(*fs, fill, 0, log_text(rng, 10));  // still writable
  fs->sync();
  fs.reset();
  expect_clean(*store);
}

TEST(Tier, RunsInTheBackgroundWhenIdle) {
  auto store = make_store();
  std::mt19937_64 rng(10);
  Files files;
  Filesystem::Options o = options();
  o.commit_interval_s = 1;
  o.gc_idle_s = 1;
  o.cold_after_s = 0;
  auto fs = Filesystem::mount(*store, o);
  InodeId ino = files.create(*fs, "f", log_text(rng, 16 * kChunk));
  fs->sync();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (fs->info().tier.chunks < 16 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_EQ(fs->info().tier.chunks, 16u);
  EXPECT_EQ(cold_chunks(*fs, ino), 16u);
  files.check(*fs);
}

TEST(Tier, NotOnFilesystemsWithoutTheFeature) {
  auto store = make_store(96 << 20, false);
  EXPECT_FALSE(load_superblock(*store).sb.features_incompat & incompat_features::cold_data);
  std::mt19937_64 rng(11);
  Files files;
  Filesystem::Options o = options();
  o.cold_after_s = 0;
  auto fs = Filesystem::mount(*store, o);
  files.create(*fs, "f", log_text(rng, 16 * kChunk));
  fs->sync();
  EXPECT_THROW(fs->recompress_cold(0), Error);
  EXPECT_FALSE(fs->info().cold_data);
  fs->collect_garbage();
  EXPECT_EQ(fs->info().tier.chunks, 0u);
  files.check(*fs);
}

TEST(Tier, ColdDataWithoutAnIdentityCanBeMoved) {
  // Without dedup, data has no BLAKE3 identity; cold, its references still
  // need SetChunkRef when GC moves it (a bug from M5 to M6 step 4: GC used
  // SetChunk, which cannot say cold, and every collection failed).
  auto store = std::make_unique<MemStore>(96 << 20);
  make_filesystem(*store, {.segment_shift = 22, .dedup = false});
  std::mt19937_64 rng(12);
  Files files;
  auto fs = Filesystem::mount(*store, options());
  for (int i = 0; i < 12; ++i) files.create(*fs, "f" + std::to_string(i), log_text(rng, 8 * kChunk));
  fs->sync();
  EXPECT_GT(fs->recompress_cold(0), 0u);
  for (int i = 0; i < 12; i += 2) files.remove(*fs, "f" + std::to_string(i));
  fs->sync();
  EXPECT_NO_THROW(fs->collect_garbage(0.97));
  EXPECT_GT(fs->info().gc.records_copied, 0u);
  files.check(*fs);
}

TEST(Tier, ColdFlagOnlyWithTheFeature) {
  // A cold reference on a filesystem without cold data is damage.
  auto io = std::make_shared<MemNodeIO>();
  Metadata m = Metadata::empty(kDataStart, 22, {.cold_data = false, .account_nodes = false}, io, 0, 0, 1);
  m.segments[0] = {1, 0};
  m.apply(op::PutInode{{.id = 2, .mode = S_IFREG | 0644, .nlink = 1}});
  ChunkRef ref{kDataStart + 4096, 100, 1000};
  ref.cold = true;
  EXPECT_THROW(m.apply(op::SetChunkRef{2, 0, ref, false}), Error);
  Metadata t = Metadata::empty(kDataStart, 22, {.account_nodes = false}, io, 0, 0, 1);
  t.segments[0] = {1, 0};
  t.apply(op::PutInode{{.id = 2, .mode = S_IFREG | 0644, .nlink = 1}});
  EXPECT_NO_THROW(t.apply(op::SetChunkRef{2, 0, ref, false}));
  // Before M6, the checkpoint said it too.
  LegacyMetadata c = LegacyMetadata::empty(kDataStart, 22, 0, 0, 1, true, true);
  c.segments[0] = {1, 0};
  c.apply(op::PutInode{{.id = 2, .mode = S_IFREG | 0644, .nlink = 1}});
  c.apply(op::SetChunkRef{2, 0, ref, false});
  Bytes snapshot = c.encode_checkpoint();
  EXPECT_EQ(LegacyMetadata::decode_checkpoint(snapshot, kDataStart, 22, true, true), c);
  EXPECT_THROW(LegacyMetadata::decode_checkpoint(snapshot, kDataStart, 22, true, false), Error);
}
