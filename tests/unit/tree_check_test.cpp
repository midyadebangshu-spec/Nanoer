// fsck and mount on a damaged metadata tree (docs/M6_PLAN.md §8): a node
// that cannot be read, a node whose record says otherwise, and derived
// items (counts, totals, unlinked inodes, back-references) that disagree
// with the files. Each is found, reported and repaired.

#include <algorithm>
#include <functional>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/check.h"
#include "nanoer/codec.h"
#include "nanoer/encoding.h"
#include "nanoer/error.h"
#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/journal.h"
#include "nanoer/mkfs.h"
#include "nanoer/record.h"

using namespace nanoer;

namespace {

Filesystem::Options options() {
  Filesystem::Options o;
  o.commit_interval_s = 0;
  o.count_lookups = false;
  o.threads = 1;
  return o;
}

constexpr int kFiles = 1500;  // enough for a tree of several 16 KiB leaves

Bytes data_for(int i, std::size_t n = 30000) {
  Bytes d(n);
  for (std::size_t j = 0; j < n; ++j) d[j] = static_cast<std::uint8_t>((j * 7 + static_cast<std::size_t>(i)) % 253);
  return d;
}

// /a and /copy (the same data: shared through dedup), /many/f0... (empty
// files, each with an attribute), unmounted cleanly; then mounted again and
// one file changed, so the two checkpoints share most of their nodes.
std::unique_ptr<MemStore> sample() {
  auto store = std::make_unique<MemStore>(64 << 20);
  make_filesystem(*store, {.segment_shift = 20, .chunk_shift = 14});
  {
    auto fs = Filesystem::mount(*store, options());
    fs->write(fs->mknod(kRootInode, "a", S_IFREG | 0644, 0, {}).ino, 0, data_for(1));
    fs->write(fs->mknod(kRootInode, "copy", S_IFREG | 0644, 0, {}).ino, 0, data_for(1));
    InodeId many = fs->mkdir(kRootInode, "many", 0755, {}).ino;
    for (int i = 0; i < kFiles; ++i) {
      InodeId f = fs->mknod(many, "f" + std::to_string(i), S_IFREG | 0644, 0, {}).ino;
      fs->setxattr(f, "user.n", as_bytes(std::to_string(i)), 0);
    }
  }
  {
    auto fs = Filesystem::mount(*store, options());
    fs->write(fs->lookup(kRootInode, "a").ino, 0, data_for(2, 100));
  }
  return store;
}

FsckReport check(Store& store) { return fsck(store, {.repair = false, .deep = true}); }
FsckReport repair(Store& store) { return fsck(store, {.repair = true, .deep = true}); }

std::string all(const std::vector<std::string>& lines) {
  std::string s;
  for (const auto& l : lines) s += l + "\n";
  return s;
}

bool mentions(const FsckReport& r, const std::string& what) {
  return std::any_of(r.errors.begin(), r.errors.end(),
                     [&](const std::string& e) { return e.find(what) != std::string::npos; });
}

// The nodes of a checkpoint's tree, with their levels.
std::map<std::uint64_t, unsigned> nodes_of(Store& store, const Superblock& sb) {
  ReplayResult r = replay(store, sb, 0);
  std::map<std::uint64_t, unsigned> out;
  r.meta.tree().for_each_node([&](const NodeRef& ref) {
    std::uint64_t seg = r.meta.segment_of(ref.offset);
    Record rec = read_record(store, ref.offset, sb.segment_offset(seg) + sb.segment_size(), r.meta.segments.at(seg).seq);
    out[ref.offset] = parse_tree_node(rec).info.level;
  });
  return out;
}

// Flips a byte of a node record's payload: its CRC no longer matches.
void damage(Store& store, std::uint64_t offset) {
  std::uint64_t at = offset + kRecordHeaderSize + kTreeNodeExtraSize + 2;
  Bytes b = store.read(at, 1);
  b[0] ^= 0x5a;
  store.write(at, b);
}

// Changes the tree as no correct writer would, and writes a checkpoint
// of it: a way to test what fsck finds in a tree whose nodes are intact.
void tamper(Store& store, const std::function<void(Metadata&)>& change) {
  Superblock sb = load_superblock(store).sb;
  ReplayResult r = replay(store, sb);
  Metadata& m = r.meta;
  Journal journal(store, sb, m.segments, std::max(sb.next_segment_seq, r.max_seq + 1));
  std::dynamic_pointer_cast<LogNodeIO>(m.io())->set_journal(&journal);
  journal.resume(r.end, m.last_txid);
  change(m);
  LogPosition at = journal.append_checkpoint(m);
  journal.flush();
  sb.checkpoint_offset = at.offset;
  sb.checkpoint_seq = at.seq;
  sb.next_segment_seq = journal.next_seq();
  write_superblock(store, sb);
  journal.checkpoint_done(at);
}

InodeId ino_of(Metadata& m, const std::string& name) { return *m.entry(kRootInode, name); }

// After a repair: checks clean, mounts read-write, and every file whose
// entry survived reads back.
void expect_repaired(Store& store) {
  FsckReport fixed = repair(store);
  EXPECT_EQ(fixed.exit_code(), 1) << all(fixed.errors);
  FsckReport after = check(store);
  EXPECT_EQ(after.exit_code(), 0) << "still wrong after repair:\n" << all(after.errors);
  auto fs = Filesystem::mount(store, options());
  EXPECT_FALSE(fs->read_only());
  fs->mknod(kRootInode, "after-repair", S_IFREG | 0644, 0, {});
}

}  // namespace

TEST(TreeFsck, CleanTreeIsClean) {
  auto store = sample();
  FsckReport r = check(*store);
  EXPECT_EQ(r.exit_code(), 0) << all(r.errors);
  LoadedSuperblock loaded = load_superblock(*store);
  ASSERT_TRUE(loaded.older);
  auto newest = nodes_of(*store, loaded.sb);
  auto older = nodes_of(*store, *loaded.older);
  EXPECT_GT(std::count_if(newest.begin(), newest.end(), [](const auto& n) { return n.second == 0; }), 3)
      << "several leaves";
  std::size_t shared = 0;
  for (const auto& [offset, level] : newest) shared += older.contains(offset);
  EXPECT_GT(shared, newest.size() / 2) << "copy on write: the checkpoints share most nodes";
}

TEST(TreeFsck, DamagedRootFallsBackToThePreviousCheckpoint) {
  auto store = sample();
  Superblock sb = load_superblock(*store).sb;
  damage(*store, replay(*store, sb, 0).meta.tree().root().offset);
  {
    // The root is read at mount: the previous checkpoint and the log after
    // it give the same state.
    auto fs = Filesystem::mount(*store, options());
    EXPECT_TRUE(fs->info().checkpoint_fallback);
    Bytes got(100);
    ASSERT_EQ(fs->read(fs->lookup(kRootInode, "a").ino, 0, got), 100u);
    EXPECT_EQ(got, data_for(2, 100));
    fs->abandon();
  }
  FsckReport r = check(*store);
  EXPECT_EQ(r.exit_code(), 4);
  EXPECT_TRUE(mentions(r, "damaged")) << all(r.errors);
  expect_repaired(*store);
  auto fs = Filesystem::mount(*store, options());
  EXPECT_EQ(fs->lookup(fs->lookup(kRootInode, "many").ino, "f" + std::to_string(kFiles - 1)).ino != 0, true);
}

TEST(TreeFsck, DamagedLeafSharedByBothCheckpointsIsSalvaged) {
  auto store = sample();
  LoadedSuperblock loaded = load_superblock(*store);
  auto newest = nodes_of(*store, loaded.sb);
  auto older = nodes_of(*store, *loaded.older);
  // A leaf both trees use, so the previous checkpoint cannot help.
  std::uint64_t leaf = 0;
  for (const auto& [offset, level] : newest) {
    if (level == 0 && older.contains(offset)) leaf = offset;
  }
  ASSERT_NE(leaf, 0u);
  damage(*store, leaf);
  FsckReport r = check(*store);
  EXPECT_EQ(r.exit_code(), 4);
  EXPECT_TRUE(mentions(r, "damaged")) << all(r.errors);
  FsckReport fixed = repair(*store);
  EXPECT_FALSE(fixed.lost.empty()) << "what the leaf held is reported lost";
  EXPECT_TRUE(std::any_of(fixed.fixed.begin(), fixed.fixed.end(),
                          [](const std::string& f) { return f.find("salvaging") != std::string::npos; }))
      << all(fixed.errors);
  FsckReport after = check(*store);
  EXPECT_EQ(after.exit_code(), 0) << all(after.errors);
  // Most files are still there.
  auto fs = Filesystem::mount(*store, options());
  EXPECT_FALSE(fs->read_only());
  std::size_t found = 0;
  for (InodeId dir : {fs->lookup(kRootInode, "many").ino}) {
    for (int i = 0; i < kFiles; ++i) {
      try {
        fs->lookup(dir, "f" + std::to_string(i));
        ++found;
      } catch (const Error&) {
      }
    }
  }
  EXPECT_GT(found, kFiles / 2u);
  fs->mknod(kRootInode, "after-salvage", S_IFREG | 0644, 0, {});
}

TEST(TreeFsck, NodeWhoseRecordSaysOtherwise) {
  auto store = sample();
  Superblock sb = load_superblock(*store).sb;
  auto nodes = nodes_of(*store, sb);
  // A leaf rewritten in place, intact, but its header names another first key.
  std::uint64_t leaf = 0;
  for (const auto& [offset, level] : nodes) {
    if (level == 0) leaf = offset;
  }
  ReplayResult r = replay(*store, sb, 0);
  std::uint64_t seg = r.meta.segment_of(leaf);
  std::uint64_t seq = r.meta.segments.at(seg).seq;
  Record rec = read_record(*store, leaf, sb.segment_offset(seg) + sb.segment_size(), seq);
  TreeNodeRecord t = parse_tree_node(rec);
  Bytes node = decode_tree_node(rec);
  EncodedChunk again = encode_chunk(node, t.codec, 1);
  NodeInfo wrong = t.info;
  wrong.first.offset += 1;
  Bytes rewritten = encode_tree_node_record(seq, again, wrong);
  ASSERT_EQ(rewritten.size(), rec.stored_size()) << "the same node compresses the same";
  store->write(leaf, rewritten);
  FsckReport found = check(*store);
  EXPECT_EQ(found.exit_code(), 4);
  EXPECT_TRUE(mentions(found, "disagrees")) << all(found.errors);
  expect_repaired(*store);
}

TEST(TreeFsck, WrongCounts) {
  auto store = sample();
  tamper(*store, [](Metadata& m) {
    Encoder e;
    e.varint(99);
    e.varint(0);
    m.tree().put({kRootInode, item_kind::counts, 0}, e.take());
  });
  EXPECT_TRUE(mentions(check(*store), "counts are wrong"));
  expect_repaired(*store);
}

TEST(TreeFsck, WrongTotals) {
  auto store = sample();
  tamper(*store, [](Metadata& m) { m.counters.logical_bytes += 4096; });
  EXPECT_TRUE(mentions(check(*store), "totals"));
  expect_repaired(*store);
}

TEST(TreeFsck, LinkedInodeListedAsUnlinked) {
  auto store = sample();
  tamper(*store, [](Metadata& m) { m.tree().put({0, item_kind::orphan, ino_of(m, "a")}, {}); });
  EXPECT_TRUE(mentions(check(*store), "unlinked inodes"));
  expect_repaired(*store);
}

TEST(TreeFsck, WrongBackReferences) {
  auto store = sample();
  tamper(*store, [](Metadata& m) {
    // /copy shares its data with /a's first version... which /a no longer
    // has; the record's users are /copy's chunks. Name the wrong one.
    InodeId copy = ino_of(m, "copy");
    std::optional<ChunkRef> ref = m.chunk(copy, 0);
    ASSERT_TRUE(ref && m.users(ref->offset));
    Encoder e;
    e.varint(1);
    e.varint(1);
    e.varint(copy);
    e.varint(7);  // a chunk /copy does not have
    m.tree().put({kRefsObject | ref->offset, item_kind::refs, 0}, e.take());
  });
  FsckReport r = check(*store);
  EXPECT_TRUE(mentions(r, "lists of users")) << all(r.errors);
  expect_repaired(*store);
}
