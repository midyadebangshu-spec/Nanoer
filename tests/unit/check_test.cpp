// nanoer fsck: for every kind of inconsistency, a test that creates it,
// checks fsck finds it, repairs it, and finds the result clean.

#include <algorithm>
#include <functional>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/journal.h"
#include "nanoer/mkfs.h"
#include "nanoer/segment.h"

using namespace nanoer;

namespace {

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 1;
  return o;
}

Bytes data_for(int i, std::size_t n = 40000) {
  Bytes d(n);
  for (std::size_t j = 0; j < n; ++j) d[j] = static_cast<std::uint8_t>((j * 13 + static_cast<std::size_t>(i)) % 251);
  return d;
}

// /a (file), /d (directory) with /d/x and /d/sub/y. Without dedup, data
// has no identity and belongs to one file.
std::unique_ptr<MemStore> sample(bool dedup = true) {
  auto store = std::make_unique<MemStore>(32 << 20);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14, .dedup = dedup});
  auto fs = Filesystem::mount(*store, options());
  InodeId a = fs->mknod(kRootInode, "a", S_IFREG | 0644, 0, {}).ino;
  fs->write(a, 0, data_for(1));
  InodeId d = fs->mkdir(kRootInode, "d", 0755, {}).ino;
  InodeId x = fs->mknod(d, "x", S_IFREG | 0644, 0, {}).ino;
  fs->write(x, 0, data_for(2));
  InodeId sub = fs->mkdir(d, "sub", 0755, {}).ino;
  InodeId y = fs->mknod(sub, "y", S_IFREG | 0644, 0, {}).ino;
  fs->write(y, 0, data_for(3, 100));
  return store;  // unmounted cleanly
}

// Writes a transaction straight into the log: a way to create states that
// the filesystem itself would never produce.
// `make` sees the metadata in memory, as fsck does.
void inject(Store& store, const std::function<std::vector<Op>(const LegacyMetadata&)>& make) {
  Superblock sb = load_superblock(store).sb;
  ReplayResult r = replay(store, sb);
  Metadata& meta = r.meta;
  Journal journal(store, sb, meta.segments, std::max(sb.next_segment_seq, r.max_seq + 1));
  journal.resume(r.end, meta.last_txid);
  std::vector<Op> ops = make(meta.to_legacy());
  for (const Op& op : ops) meta.apply(op);
  journal.append_transaction(meta.last_txid + 1, ops);
  journal.flush();
}

InodeId find(const LegacyMetadata& m, InodeId dir, const std::string& name) {
  return m.find(dir)->entries.at(name);
}

FsckReport check(Store& store, bool deep = false) { return fsck(store, {.repair = false, .deep = deep}); }
FsckReport repair(Store& store, bool deep = false) { return fsck(store, {.repair = true, .deep = deep}); }

std::string all(const std::vector<std::string>& lines) {
  std::string s;
  for (const auto& l : lines) s += l + "\n";
  return s;
}

// The standard sequence: an error is found, repaired, and gone afterwards.
void expect_found_and_repaired(Store& store, const std::string& what, bool deep = false) {
  FsckReport before = check(store, deep);
  ASSERT_EQ(before.exit_code(), 4) << what << " was not found";
  bool mentioned = std::any_of(before.errors.begin(), before.errors.end(),
                               [&](const std::string& e) { return e.find(what) != std::string::npos; });
  EXPECT_TRUE(mentioned) << "expected an error about '" << what << "', got:\n" << all(before.errors);
  FsckReport fixed = repair(store, deep);
  EXPECT_EQ(fixed.exit_code(), 1) << all(fixed.errors);
  FsckReport after = check(store, deep);
  EXPECT_EQ(after.exit_code(), 0) << "still wrong after repair:\n" << all(after.errors);
  // And it mounts, read-write, and works.
  auto fs = Filesystem::mount(store, options());
  EXPECT_FALSE(fs->read_only());
  EXPECT_FALSE(fs->info().errors_flag);
  static int repairs = 0;
  fs->mknod(kRootInode, "after-repair-" + std::to_string(++repairs), S_IFREG | 0644, 0, {});
}

}  // namespace

TEST(Fsck, CleanFilesystemIsClean) {
  auto store = sample();
  FsckReport r = check(*store, true);
  EXPECT_EQ(r.exit_code(), 0) << all(r.errors);
  EXPECT_EQ(r.files, 3u);
  EXPECT_EQ(r.directories, 3u);
  EXPECT_EQ(r.chunks_verified, r.chunks);
  EXPECT_EQ(repair(*store).exit_code(), 0) << "nothing to repair: must write nothing";
}

TEST(Fsck, WrongLinkCount) {
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    InodeAttrs a = *m.find(find(m, kRootInode, "a"));
    a.nlink = 5;
    return std::vector<Op>{op::PutInode{a}};
  });
  expect_found_and_repaired(*store, "link count is 5, should be 1");
}

TEST(Fsck, EntryPointingAtMissingInode) {
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    return std::vector<Op>{op::DeleteInode{find(m, kRootInode, "a")}};  // entry left behind
  });
  expect_found_and_repaired(*store, "which does not exist");
  auto fs = Filesystem::mount(*store, options());
  EXPECT_THROW(fs->lookup(kRootInode, "a"), Error);
}

TEST(Fsck, UnreachableTreeGoesToLostAndFound) {
  auto store = sample();
  InodeId d = 0;
  inject(*store, [&](const LegacyMetadata& m) {
    d = find(m, kRootInode, "d");
    return std::vector<Op>{op::RemoveEntry{kRootInode, "d"}};  // /d, with contents, detached
  });
  expect_found_and_repaired(*store, "is in no directory reachable from /");
  auto fs = Filesystem::mount(*store, options());
  InodeId lf = fs->lookup(kRootInode, "lost+found").ino;
  InodeId back = fs->lookup(lf, "#" + std::to_string(d)).ino;
  EXPECT_EQ(back, d);
  InodeId y = fs->lookup(fs->lookup(d, "sub").ino, "y").ino;  // contents came along
  Bytes buf(100);
  fs->read(y, 0, buf);
  EXPECT_EQ(buf, data_for(3, 100));
  EXPECT_EQ(fs->lookup(d, "..").ino, lf);
}

TEST(Fsck, DirectoryWithTwoNames) {
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    return std::vector<Op>{op::SetEntry{kRootInode, "alias", find(m, kRootInode, "d")}};
  });
  expect_found_and_repaired(*store, "a directory may have only one name");
}

TEST(Fsck, WrongDirectoryParent) {
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    InodeAttrs a = *m.find(find(m, find(m, kRootInode, "d"), "sub"));
    a.parent = kRootInode;
    return std::vector<Op>{op::PutInode{a}};
  });
  expect_found_and_repaired(*store, "has the wrong parent");
}

TEST(Fsck, ChunkPastTheEndOfAFile) {
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    InodeAttrs a = *m.find(find(m, kRootInode, "a"));
    a.size = 100;  // chunks 1 and 2 are still there
    return std::vector<Op>{op::PutInode{a}};
  });
  expect_found_and_repaired(*store, "past the end of the file");
}

TEST(Fsck, ChunkBorrowedFromAnotherFile) {
  auto store = sample(false);
  inject(*store, [](const LegacyMetadata& m) {
    const Inode& a = *m.find(find(m, kRootInode, "a"));
    InodeId x = find(m, find(m, kRootInode, "d"), "x");
    return std::vector<Op>{op::SetChunk{x, 0, a.chunks.at(0)}};
  });
  expect_found_and_repaired(*store, "belongs to another file");
}

TEST(Fsck, SharingDataWithoutAnIdentity) {
  // Only data with a BLAKE3 identity may be shared (dedup); other data
  // belongs to the file it was written for.
  auto store = sample(false);
  inject(*store, [](const LegacyMetadata& m) {
    const Inode& a = *m.find(find(m, kRootInode, "a"));
    InodeId x = find(m, find(m, kRootInode, "d"), "x");
    return std::vector<Op>{op::SetChunkRef{x, 0, a.chunks.at(0), true, ChunkUser{a.id, 0}}};
  });
  expect_found_and_repaired(*store, "belongs to another file");
}

TEST(Fsck, SharedDataIsClean) {
  // Data with an identity, shared by two files, as dedup does it.
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    const Inode& a = *m.find(find(m, kRootInode, "a"));
    InodeId x = find(m, find(m, kRootInode, "d"), "x");
    EXPECT_NE(a.chunks.at(0).hash, 0u);
    return std::vector<Op>{op::SetChunkRef{x, 0, a.chunks.at(0), true, ChunkUser{a.id, 0}}};
  });
  FsckReport report = check(*store, true);
  EXPECT_EQ(report.exit_code(), 0) << all(report.errors);
}

TEST(Fsck, WrongDedupKey) {
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    InodeId x = find(m, find(m, kRootInode, "d"), "x");
    ChunkRef ref = m.find(x)->chunks.at(1);
    ref.hash ^= 0x100;  // the record's own BLAKE3 says otherwise
    return std::vector<Op>{op::SetChunkRef{x, 1, ref, false}};
  });
  expect_found_and_repaired(*store, "identity does not match");
}

TEST(Fsck, FilesystemWithoutSharedRecords) {
  // As M3 made them: still mounts, works and checks clean.
  MemStore store(32 << 20);
  make_filesystem(store, {.segment_shift = 20, .chunk_shift = 14, .shared_records = false});
  {
    auto fs = Filesystem::mount(store, options());
    InodeId a = fs->mknod(kRootInode, "a", S_IFREG | 0644, 0, {}).ino;
    fs->write(a, 0, data_for(1));
  }
  EXPECT_FALSE(has_shared_records(load_superblock(store).sb));
  EXPECT_EQ(check(store, true).exit_code(), 0);
  auto fs = Filesystem::mount(store, options());
  Bytes back(40000);
  EXPECT_EQ(fs->read(fs->lookup(kRootInode, "a").ino, 0, back), back.size());
  EXPECT_EQ(back, data_for(1));
}

TEST(Fsck, DamagedChunkHeaderAndPayload) {
  auto store = sample();
  // Find the chunk records first: a scan stops at the first damaged record.
  Superblock sb = load_superblock(*store).sb;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> chunks;
  for (std::uint64_t i = 0; i < sb.segment_count; ++i) {
    SegmentScanner s(*store, sb, i);
    while (auto r = s.next()) {
      if (r->header.type == RecordType::data_chunk) chunks.emplace_back(r->offset, r->stored_size());
    }
  }
  auto flip_in_chunk = [&](int which, std::uint64_t at_from_end) {
    auto [off, size] = chunks.at(static_cast<std::size_t>(which));
    Bytes b = store->read(at_from_end ? off + size - at_from_end : off + 40, 1);
    b[0] ^= 0x55;
    store->write(at_from_end ? off + size - at_from_end : off + 40, b);
  };
  flip_in_chunk(0, 0);  // header: found by a normal check
  expect_found_and_repaired(*store, "missing or damaged");
  FsckReport clean = check(*store);
  flip_in_chunk(1, 30);  // payload: only a deep check reads it
  EXPECT_EQ(check(*store).exit_code(), 0) << "a normal check reads headers only";
  FsckReport deep = check(*store, true);
  EXPECT_EQ(deep.exit_code(), 4);
  ASSERT_FALSE(deep.lost.empty());
  EXPECT_NE(deep.lost.back().find("bytes"), std::string::npos) << "lost data is listed by range";
  expect_found_and_repaired(*store, "damaged", true);
}

TEST(Fsck, MidLogDamageKeepsTheIntactPrefix) {
  auto store = std::make_unique<MemStore>(32 << 20);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14});
  {
    auto fs = Filesystem::mount(*store, options());
    for (int i = 0; i < 6; ++i) {
      fs->mknod(kRootInode, "f" + std::to_string(i), S_IFREG | 0644, 0, {});
      fs->sync();
    }
    fs->abandon();
  }
  Superblock sb = load_superblock(*store).sb;
  std::vector<std::uint64_t> txns;
  for (std::uint64_t i = 0; i < sb.segment_count; ++i) {
    SegmentScanner s(*store, sb, i);
    while (auto r = s.next()) {
      if (r->header.type == RecordType::metadata_log) txns.push_back(r->offset + 40);
    }
  }
  Bytes b = store->read(txns.at(txns.size() - 3), 1);
  b[0] ^= 1;
  store->write(txns.at(txns.size() - 3), b);
  FsckReport r = check(*store);
  ASSERT_EQ(r.exit_code(), 4);
  EXPECT_FALSE(r.lost.empty());
  expect_found_and_repaired(*store, "the log is damaged");
  auto fs = Filesystem::mount(*store, options());
  EXPECT_EQ(fs->list(kRootInode).size(), 2u + 3u + 1u);  // f0..f2, plus after-repair
}

TEST(Fsck, ContradictoryTransaction) {
  auto store = sample();
  // A transaction whose second operation cannot be applied after the first.
  inject(*store, [](const LegacyMetadata& m) {
    InodeId a = find(m, kRootInode, "a");
    return std::vector<Op>{op::SetEntry{kRootInode, "b", a}};
  });
  {
    // Corrupt it in meaning, not in bytes: overwrite the transaction with one
    // that removes an entry that does not exist, written the same way.
    Superblock sb = load_superblock(*store).sb;
    ReplayResult r = replay(*store, sb);
    Metadata& meta = r.meta;
    Journal journal(*store, sb, meta.segments, std::max(sb.next_segment_seq, r.max_seq + 1));
    journal.resume(r.end, meta.last_txid);
    journal.append_transaction(meta.last_txid + 1, {op::RemoveEntry{kRootInode, "nonexistent"}});
    journal.flush();
  }
  auto fs = Filesystem::mount(*store, options());
  EXPECT_TRUE(fs->read_only()) << "mount must not write past a contradiction";
  EXPECT_TRUE(fs->info().log_damaged);
  fs.reset();
  expect_found_and_repaired(*store, "contradicts itself");
}

TEST(Fsck, DamagedNewestCheckpoint) {
  auto store = sample();
  { auto fs = Filesystem::mount(*store, options()); }  // a second checkpoint
  Superblock sb = load_superblock(*store).sb;
  Bytes b = store->read(sb.checkpoint_offset + 50, 1);
  b[0] ^= 0x10;
  store->write(sb.checkpoint_offset + 50, b);
  expect_found_and_repaired(*store, "the newest checkpoint is damaged");
}

TEST(Fsck, BothSuperblocksLostAreRebuilt) {
  auto store = sample();
  { auto fs = Filesystem::mount(*store, options()); }
  store->write(0, Bytes(2 * kBlockSize, 0));  // both superblocks gone
  EXPECT_THROW(Filesystem::mount(*store, options()), Error);
  FsckReport r = check(*store);
  EXPECT_EQ(r.exit_code(), 4);
  FsckReport fixed = repair(*store);
  EXPECT_EQ(fixed.exit_code(), 1) << all(fixed.errors);
  EXPECT_EQ(check(*store).exit_code(), 0);
  Superblock sb = load_superblock(*store).sb;
  EXPECT_EQ(sb.segment_size(), 1u << 20) << "segment size recovered";
  EXPECT_EQ(sb.chunk_size(), 16u << 10) << "chunk size recovered";
  auto fs = Filesystem::mount(*store, options());
  Bytes buf(40000);
  fs->read(fs->lookup(kRootInode, "a").ino, 0, buf);
  EXPECT_EQ(buf, data_for(1));
}

TEST(Fsck, OrphansAreFreed) {
  auto store = sample();
  inject(*store, [](const LegacyMetadata& m) {
    InodeAttrs a = *m.find(find(m, kRootInode, "a"));
    a.nlink = 0;
    return std::vector<Op>{op::RemoveEntry{kRootInode, "a"}, op::PutInode{a}};
  });
  FsckReport r = check(*store);
  EXPECT_EQ(r.exit_code(), 0) << "an orphan is a normal crash leftover, not an error";
  ASSERT_FALSE(r.notes.empty());
  repair(*store);
  EXPECT_TRUE(check(*store).notes.empty());
}

TEST(Fsck, ClearsTheErrorsFlag) {
  auto store = sample();
  Superblock sb = load_superblock(*store).sb;
  sb.state |= state_flags::errors;
  write_superblock(*store, sb);
  FsckReport r = check(*store);
  EXPECT_EQ(r.exit_code(), 0);
  EXPECT_EQ(repair(*store).exit_code(), 1);
  EXPECT_FALSE(load_superblock(*store).sb.state & state_flags::errors);
}

TEST(Fsck, ColdDataIsClean) {
  // Recompressed records and in-place marks, as tiering makes them;
  // and a superblock rebuilt from them keeps the feature.
  auto store = sample();
  {
    auto fs = Filesystem::mount(*store, options());
    EXPECT_GT(fs->recompress_cold(0), 0u);
  }
  FsckReport report = check(*store, true);
  EXPECT_EQ(report.exit_code(), 0) << all(report.errors);
  store->write(0, Bytes(2 * kBlockSize, 0));
  EXPECT_EQ(repair(*store).exit_code(), 1);
  EXPECT_TRUE(has_cold_data(load_superblock(*store).sb));
  EXPECT_EQ(check(*store, true).exit_code(), 0);
  auto fs = Filesystem::mount(*store, options());
  Bytes buf(40000);
  fs->read(fs->lookup(kRootInode, "a").ino, 0, buf);
  EXPECT_EQ(buf, data_for(1));
}

TEST(Fsck, DamagedColdRecord) {
  auto store = sample();
  {
    auto fs = Filesystem::mount(*store, options());
    fs->recompress_cold(0);
  }
  Superblock sb = load_superblock(*store).sb;
  LegacyMetadata m = replay(*store, sb).meta.to_legacy();
  const Inode& a = *m.find(find(m, kRootInode, "a"));
  const ChunkRef ref = a.chunks.at(1);
  ASSERT_TRUE(ref.cold);
  Bytes b = store->read(ref.offset + ref.stored_size - 30, 1);
  b[0] ^= 0x55;
  store->write(ref.offset + ref.stored_size - 30, b);
  FsckReport deep = check(*store, true);
  EXPECT_EQ(deep.exit_code(), 4);
  // Reported by file and byte range.
  std::size_t ranges = 0;
  for (const std::string& l : deep.lost) ranges += l.find("/a") != std::string::npos && l.find("bytes") != std::string::npos;
  EXPECT_EQ(ranges, 1u) << all(deep.lost);
  expect_found_and_repaired(*store, "damaged", true);
}
