// The tree-backed metadata against the in-memory metadata of M2-M5 as an
// oracle: random operations applied to both must give the same files,
// entries, chunks, attributes, space accounting and counters, through
// checkpoints and back (docs/M6_PLAN.md §3). Also back-references, sticky
// reference items and the conversions between the two.

#include "nanoer/metadata.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/error.h"
#include "nanoer/legacy_metadata.h"

using namespace nanoer;

namespace {

constexpr std::uint64_t kDataStart = 1 << 20;
constexpr unsigned kShift = 20;
constexpr std::uint64_t kSegments = 256;

// Small nodes, so that a few hundred files make a tree of several levels.
Metadata::Options options() {
  Metadata::Options o;
  o.tree = {.node_bytes = 512, .cache_bytes = 1 << 20};
  o.account_nodes = false;  // nodes in memory: the segments hold data only
  return o;
}

InodeAttrs attrs(InodeId id, std::uint32_t mode) {
  InodeAttrs a;
  a.id = id;
  a.mode = mode;
  a.nlink = S_ISDIR(mode) ? 2 : 1;
  a.parent = kRootInode;
  a.mtime_ns = -7;
  return a;
}

// Both metadata, and random operations that make sense on them.
struct Pair {
  explicit Pair(std::uint64_t seed)
      : io(std::make_shared<MemNodeIO>()),
        tree(Metadata::empty(kDataStart, kShift, options(), io, 0, 0, 1)),
        legacy(LegacyMetadata::empty(kDataStart, kShift, 0, 0, 1, true, true)),
        rng(seed) {
    for (std::uint64_t i = 0; i < kSegments; ++i) {
      tree.segments[i] = legacy.segments[i] = {i + 1, 0};
    }
    dirs.push_back(kRootInode);
  }

  void apply(const Op& op) {
    legacy.apply(op);
    tree.apply(op);
    ++applied;
  }

  std::uint64_t pick(std::uint64_t n) { return rng() % n; }
  ChunkRef fresh(bool hashed) {
    ChunkRef r{kDataStart + next_record++ * 4096, static_cast<std::uint32_t>(100 + pick(3000)), 4096};
    if (hashed) r.hash = rng() | 1;
    if (hashed && pick(4) == 0) r.cold = true;
    return r;
  }
  // The chunks that refer to the record at `offset`.
  std::vector<ChunkUser> users_of(std::uint64_t offset) const {
    std::vector<ChunkUser> out;
    for (const auto& [id, inode] : legacy.inodes) {
      for (const auto& [index, ref] : inode.chunks) {
        if (ref.offset == offset) out.push_back({id, index});
      }
    }
    return out;
  }
  std::string name() { return "n" + std::to_string(pick(60)); }

  void step() {
    int what = static_cast<int>(pick(100));
    if (what < 12 || files.empty()) {  // a new file or directory
      InodeId id = legacy.next_inode;
      bool dir = pick(4) == 0;
      InodeId parent = dirs[pick(dirs.size())];
      std::string n = name();
      if (legacy.find(parent)->entries.contains(n)) return;
      apply(op::PutInode{attrs(id, dir ? S_IFDIR | 0755 : S_IFREG | 0644)});
      apply(op::SetEntry{parent, n, id});
      (dir ? dirs : files).push_back(id);
      where[id] = {parent, n};
      return;
    }
    InodeId f = files[pick(files.size())];
    std::uint64_t index = pick(8);
    const Inode& file = *legacy.find(f);
    if (what < 35) {  // write a chunk
      if (pick(2)) {
        apply(op::SetChunk{f, index, fresh(false)});
      } else {
        apply(op::SetChunkRef{f, index, fresh(true), false});
      }
    } else if (what < 45) {  // share a record, as dedup does
      std::vector<ChunkRef> hashed;
      for (InodeId g : files) {
        for (const auto& [i, ref] : legacy.find(g)->chunks) {
          if (ref.hash && !ref.packed) hashed.push_back(ref);
        }
      }
      if (hashed.empty()) return;
      ChunkRef ref = hashed[pick(hashed.size())];
      if (auto it = file.chunks.find(index); it != file.chunks.end() && it->second.offset == ref.offset) return;
      std::vector<ChunkUser> users = users_of(ref.offset);
      apply(op::SetChunkRef{f, index, ref, true, users.front()});
    } else if (what < 50) {  // a pack of small files
      ChunkRef pack = fresh(false);
      pack.packed = true;
      for (int piece = 0; piece < 3; ++piece) {
        ChunkRef p = pack;
        p.piece = static_cast<std::uint32_t>(piece * 1000);
        p.size = 900;
        if (pick(2)) p.hash = rng() | 1;
        apply(op::SetChunkRef{files[pick(files.size())], 0, p, false});
      }
    } else if (what < 58) {
      apply(op::RemoveChunk{f, index});
    } else if (what < 62) {
      apply(op::TruncateChunks{f, index});
    } else if (what < 68) {
      InodeId any = pick(2) ? f : dirs[pick(dirs.size())];
      apply(op::SetXattr{any, "user." + name(), Bytes(pick(40), static_cast<std::uint8_t>(pick(256)))});
    } else if (what < 71) {
      if (!file.xattrs.empty()) apply(op::RemoveXattr{f, file.xattrs.begin()->first});
    } else if (what < 77) {  // attributes change
      InodeAttrs a = file;
      a.size = pick(1 << 20);
      a.uid = static_cast<std::uint32_t>(pick(5));
      apply(op::PutInode{a});
    } else if (what < 83) {  // rename
      auto [dir, old] = where[f];
      InodeId to = dirs[pick(dirs.size())];
      std::string n = name();
      if (legacy.find(to)->entries.contains(n)) return;
      apply(op::SetEntry{to, n, f});
      apply(op::RemoveEntry{dir, old});
      where[f] = {to, n};
    } else if (what < 90) {  // unlink: an orphan first, freed at once or later
      auto [dir, old] = where[f];
      apply(op::RemoveEntry{dir, old});
      InodeAttrs a = file;
      a.nlink = 0;
      apply(op::PutInode{a});
      files.erase(std::find(files.begin(), files.end(), f));
      where.erase(f);
      if (pick(2)) {
        apply(op::DeleteInode{f});
      } else {
        orphans.push_back(f);
      }
    } else if (!orphans.empty()) {
      apply(op::DeleteInode{orphans.back()});
      orphans.pop_back();
    }
  }

  // What the tree says agrees with the oracle, item by item and through
  // its readers.
  void check(const std::string& when) {
    LegacyMetadata seen = tree.to_legacy();
    ASSERT_EQ(seen.inodes, legacy.inodes) << when;
    ASSERT_EQ(seen.segments, legacy.segments) << when;
    ASSERT_EQ(seen.next_inode, legacy.next_inode) << when;
    if (!tree.same_as(legacy)) {
      // Where they differ, for the failure message.
      Metadata want = Metadata::from_legacy(legacy, options(), std::make_shared<MemNodeIO>());
      MetaCounters c = tree.counters;
      c.node_bytes = 0;
      ASSERT_EQ(c, want.counters) << when;
      ASSERT_EQ(tree.last_txid, want.last_txid) << when;
      std::vector<std::pair<TreeKey, Bytes>> a, b;
      auto collect = [](const Metadata& m, std::vector<std::pair<TreeKey, Bytes>>& out) {
        m.scan({}, [&](const TreeKey& k, ByteView v) {
          out.emplace_back(k, Bytes(v.begin(), v.end()));
          return true;
        });
      };
      collect(tree, a);
      collect(want, b);
      for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        if (i >= a.size() || i >= b.size() || a[i] != b[i]) {
          const TreeKey& k = i < a.size() ? a[i].first : b[i].first;
          FAIL() << when << ": items differ at " << i << " of " << a.size() << "/" << b.size() << ", key (" << k.object
                 << ", " << int(k.kind) << ", " << k.offset << ")";
        }
      }
      FAIL() << when << ": differ";
    }
    std::set<InodeId> orphaned(orphans.begin(), orphans.end());
    std::vector<InodeId> got = tree.orphans();
    ASSERT_EQ(std::set<InodeId>(got.begin(), got.end()), orphaned) << when;
    for (const auto& [id, inode] : legacy.inodes) {
      ASSERT_EQ(*tree.attrs(id), static_cast<const InodeAttrs&>(inode)) << when;
      for (const auto& [n, child] : inode.entries) ASSERT_EQ(tree.entry(id, n), child) << when;
      for (const auto& [n, value] : inode.xattrs) ASSERT_EQ(tree.xattr(id, n), value) << when;
      std::map<std::uint64_t, ChunkRef> chunks;
      tree.chunks(id, 0, [&](std::uint64_t i, const ChunkRef& r) {
        chunks.emplace(i, r);
        return true;
      });
      ASSERT_EQ(chunks, inode.chunks) << when;
      for (const auto& [i, ref] : inode.chunks) {
        ASSERT_EQ(tree.references(ref), legacy.references(ref)) << when;
        // Back-references: every user of a pack or a shared record.
        if (std::optional<std::vector<ChunkUser>> users = tree.users(ref.offset)) {
          std::vector<ChunkUser> want = users_of(ref.offset);
          auto order = [](const ChunkUser& a, const ChunkUser& b) {
            return std::pair(a.ino, a.index) < std::pair(b.ino, b.index);
          };
          std::sort(users->begin(), users->end(), order);
          std::sort(want.begin(), want.end(), order);
          ASSERT_EQ(*users, want) << when;
        } else {
          ASSERT_FALSE(ref.packed) << when;
          ASSERT_EQ(users_of(ref.offset).size(), 1u) << when;
        }
      }
    }
  }

  // A checkpoint, and the tree read back from it.
  void checkpoint() {
    tree.flush();
    Bytes cp = tree.encode_checkpoint();
    Metadata back = Metadata::decode_checkpoint(cp, kDataStart, kShift, options(), io);
    ASSERT_EQ(back.counters, tree.counters);
    ASSERT_TRUE(back.same_as(tree));
    tree = std::move(back);
  }

  std::shared_ptr<MemNodeIO> io;
  Metadata tree;
  LegacyMetadata legacy;
  std::mt19937_64 rng;
  std::vector<InodeId> dirs, files, orphans;
  std::map<InodeId, std::pair<InodeId, std::string>> where;  // file -> its entry
  std::uint64_t next_record = 1;
  std::uint64_t applied = 0;
};

}  // namespace

TEST(TreeMetadata, RandomOperationsMatchTheLegacyMetadata) {
  for (std::uint64_t seed : {1, 2, 3, 4}) {
    Pair p(seed);
    for (int step = 0; step < 3000; ++step) {
      p.step();
      if (step % 150 == 0) {
        p.check("seed " + std::to_string(seed) + ", step " + std::to_string(step));
        if (testing::Test::HasFatalFailure()) return;
      }
      if (step % 400 == 399) p.checkpoint();
    }
    p.check("seed " + std::to_string(seed) + " at the end");
    EXPECT_GE(p.tree.tree().height(), 3u) << "small nodes: inner levels too";
    EXPECT_GT(p.applied, 3000u);
    // The conversions: the tree as it would be built from the legacy
    // metadata, and back.
    Metadata built = Metadata::from_legacy(p.legacy, options(), std::make_shared<MemNodeIO>());
    EXPECT_TRUE(built.same_as(p.legacy));
    LegacyMetadata round = built.to_legacy();
    EXPECT_EQ(round.inodes, p.legacy.inodes);
    EXPECT_EQ(round.segments, p.legacy.segments);
    EXPECT_EQ(p.tree.to_legacy().inodes, p.legacy.inodes);
  }
}

TEST(TreeMetadata, SharedRecordsKeepTheirUsers) {
  Pair p(9);
  p.apply(op::PutInode{attrs(2, S_IFREG | 0644)});
  p.apply(op::PutInode{attrs(3, S_IFREG | 0644)});
  ChunkRef r = p.fresh(true);
  p.apply(op::SetChunkRef{2, 0, r, false});
  EXPECT_FALSE(p.tree.users(r.offset)) << "one user: the record's header names it";
  // Shared for the first time: the owner the header names must be given.
  EXPECT_THROW(p.tree.apply(op::SetChunkRef{3, 0, r, true}), Error);
  p.apply(op::SetChunkRef{3, 0, r, true, ChunkUser{2, 0}});
  ASSERT_TRUE(p.tree.users(r.offset));
  EXPECT_EQ(p.tree.users(r.offset)->size(), 2u);
  EXPECT_EQ(p.tree.references(r), 2u);
  // Down to one user, which may not be the one the header names: the refs
  // item stays (sticky), so GC still finds the user.
  p.apply(op::RemoveChunk{2, 0});
  ASSERT_TRUE(p.tree.users(r.offset));
  EXPECT_EQ(*p.tree.users(r.offset), (std::vector<ChunkUser>{{3, 0}}));
  EXPECT_EQ(p.tree.references(r), 1u);
  // The space goes with the last reference.
  std::uint64_t seg = p.tree.segment_of(r.offset);
  EXPECT_EQ(p.tree.segments.at(seg).live_bytes, r.stored_size);
  p.apply(op::RemoveChunk{3, 0});
  EXPECT_FALSE(p.tree.users(r.offset));
  EXPECT_EQ(p.tree.segments.at(seg).live_bytes, 0u);
  // fsck sees the sticky item as a shared record with one reference.
  p.apply(op::SetChunkRef{2, 1, r, false});
  p.apply(op::SetChunkRef{3, 1, r, true, ChunkUser{2, 1}});
  p.apply(op::RemoveChunk{2, 1});
  EXPECT_EQ(p.tree.to_legacy().shared.at(r.offset), 1u);
}

TEST(TreeMetadata, ChunkWalkInSteps) {
  Pair p(5);
  for (int i = 0; i < 2000; ++i) p.step();
  std::vector<std::tuple<InodeId, std::uint64_t, ChunkRef>> all, stepped;
  p.tree.for_each_chunk([&](InodeId ino, std::uint64_t index, const ChunkRef& r) {
    all.emplace_back(ino, index, r);
    return true;
  });
  ASSERT_GT(all.size(), 40u);
  std::optional<TreeKey> at = TreeKey{};
  int steps = 0;
  while (at) {
    at = p.tree.chunks_from(*at, 7, [&](InodeId ino, std::uint64_t index, const ChunkRef& r) {
      stepped.emplace_back(ino, index, r);
    });
    ++steps;
  }
  EXPECT_EQ(stepped, all);
  EXPECT_GT(steps, 10);
}

TEST(TreeMetadata, InconsistentOperationsAreCorrupt) {
  Pair p(3);
  p.apply(op::PutInode{attrs(2, S_IFREG | 0644)});
  ChunkRef r = p.fresh(false);
  p.apply(op::SetChunk{2, 0, r});
  // (A second plain reference to a plain record is not caught here, as
  // before M6: it costs a lookup per write. fsck finds it.)
  ChunkRef h = p.fresh(true);
  p.apply(op::SetChunkRef{2, 1, h, false});
  p.apply(op::PutInode{attrs(3, S_IFREG | 0644)});
  p.apply(op::SetChunkRef{3, 1, h, true, ChunkUser{2, 1}});
  EXPECT_THROW(p.tree.apply(op::SetChunkRef{3, 2, h, false}), Error) << "a new chunk where a shared one is";
  EXPECT_THROW(p.tree.apply(op::SetChunk{9, 0, p.fresh(false)}), Error) << "a missing inode";
  EXPECT_THROW(p.tree.apply(op::SetEntry{2, "x", 2}), Error) << "an entry in a file";
  EXPECT_THROW(p.tree.apply(op::RemoveEntry{kRootInode, "missing"}), Error);
  EXPECT_THROW(p.tree.apply(op::RemoveXattr{2, "user.missing"}), Error);
  EXPECT_THROW(p.tree.apply(op::DeleteInode{kRootInode}), Error);
  ChunkRef piece = p.fresh(false);
  piece.packed = true;
  EXPECT_THROW(p.tree.apply(op::SetChunkRef{2, 3, piece, true}), Error) << "a pack piece marked shared";
  ChunkRef outside{kDataStart + (kSegments + 5) * (1 << kShift), 10, 10};
  EXPECT_THROW(p.tree.apply(op::SetChunk{2, 4, outside}), Error) << "a segment not in use";
}

TEST(TreeMetadata, CheckpointDamageIsDetected) {
  Pair p(4);
  for (int i = 0; i < 300; ++i) p.step();
  p.tree.flush();
  Bytes cp = p.tree.encode_checkpoint();
  EXPECT_TRUE(Metadata::is_tree_checkpoint(cp));
  EXPECT_FALSE(LegacyMetadata::is_v2_checkpoint(cp));
  EXPECT_NO_THROW(Metadata::decode_checkpoint(cp, kDataStart, kShift, options(), p.io));
  for (std::size_t cut : {std::size_t{4}, std::size_t{12}, cp.size() - 1}) {
    EXPECT_THROW(Metadata::decode_checkpoint(ByteView(cp).first(cut), kDataStart, kShift, options(), p.io), Error)
        << cut;
  }
  Bytes longer = cp;
  longer.push_back(0);
  EXPECT_THROW(Metadata::decode_checkpoint(longer, kDataStart, kShift, options(), p.io), Error);
  // Not flushed: no checkpoint.
  p.tree.apply(op::SetXattr{kRootInode, "user.late", {1}});
  EXPECT_THROW(p.tree.encode_checkpoint(), Error);
}
