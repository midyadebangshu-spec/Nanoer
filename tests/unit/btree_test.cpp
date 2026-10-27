// The copy-on-write B+tree on its own: against a std::map model, copy on
// write (old roots stay readable, exactly the replaced nodes released),
// splits and merges at every level, relocation, the cache's budget, and
// damaged nodes (docs/M6_PLAN.md §3).

#include <map>
#include <random>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "nanoer/btree.h"
#include "nanoer/error.h"

using namespace nanoer;

namespace {

// Nodes in memory. Released nodes stay readable (as on disk, until their
// segment is reused), but are no longer live.
class MemNodeStore final : public NodeStore {
 public:
  Bytes read(const NodeRef& ref) override {
    auto it = nodes_.find(ref.offset);
    if (it == nodes_.end() || it->second.size() != ref.stored_size) throw Error(Errc::corrupt, "no such node");
    ++reads;
    return it->second;
  }
  NodeRef write(ByteView encoded, const NodeInfo& info) override {
    std::uint64_t at = next_;
    next_ += 1000;
    nodes_[at].assign(encoded.begin(), encoded.end());
    infos_[at] = info;
    live.insert(at);
    ++writes;
    return {at, static_cast<std::uint32_t>(encoded.size())};
  }
  void release(const NodeRef& ref) override {
    ASSERT_TRUE(live.erase(ref.offset)) << "released a node that was not live: " << ref.offset;
  }
  const NodeInfo& info(std::uint64_t offset) const { return infos_.at(offset); }
  NodeRef ref(std::uint64_t offset) const {
    return {offset, static_cast<std::uint32_t>(nodes_.at(offset).size())};
  }
  void corrupt(std::uint64_t offset) { nodes_[offset][0] ^= 0xff; }

  std::set<std::uint64_t> live;
  std::uint64_t reads = 0, writes = 0;

 private:
  std::map<std::uint64_t, Bytes> nodes_;
  std::map<std::uint64_t, NodeInfo> infos_;
  std::uint64_t next_ = 4096;
};

// Every node reachable from `root`.
std::set<std::uint64_t> reachable(MemNodeStore& store, NodeRef root) {
  std::set<std::uint64_t> out;
  std::vector<NodeRef> todo;
  if (root) todo.push_back(root);
  while (!todo.empty()) {
    NodeRef r = todo.back();
    todo.pop_back();
    out.insert(r.offset);
    BTree::Decoded d = BTree::decode(store.read(r));
    for (const NodeRef& c : d.children) todo.push_back(c);
  }
  return out;
}

TreeKey key(std::uint64_t i) { return {i / 7, static_cast<std::uint8_t>(i % 7), i * 13}; }
Bytes value(std::uint64_t i, std::size_t n) {
  Bytes v(n);
  for (std::size_t j = 0; j < n; ++j) v[j] = static_cast<std::uint8_t>(i + j);
  return v;
}

std::map<TreeKey, Bytes> contents(const BTree& t) {
  std::map<TreeKey, Bytes> out;
  t.scan({}, kMaxTreeKey, [&](const TreeKey& k, ByteView v) {
    out.emplace(k, Bytes(v.begin(), v.end()));
    return true;
  });
  return out;
}

const BTree::Options kSmall{.node_bytes = 512, .cache_bytes = 1 << 20};

}  // namespace

TEST(BTree, EmptyPutGetEraseFlush) {
  MemNodeStore store;
  BTree t(store, kSmall);
  EXPECT_FALSE(t.get(key(1)));
  EXPECT_FALSE(t.erase(key(1)));
  EXPECT_FALSE(t.flush());  // nothing to write
  t.put(key(1), value(1, 10));
  t.put(key(1), value(2, 20));  // replaced
  EXPECT_EQ(*t.get(key(1)), value(2, 20));
  NodeRef root = t.flush();
  ASSERT_TRUE(root);
  BTree again(store, kSmall, root);
  EXPECT_EQ(*again.get(key(1)), value(2, 20));
  EXPECT_TRUE(again.erase(key(1)));
  EXPECT_FALSE(again.get(key(1)));
  EXPECT_FALSE(again.flush()) << "an empty tree has no root";
  EXPECT_TRUE(store.live.empty());
}

TEST(BTree, RandomOperationsMatchAMap) {
  for (std::uint64_t seed : {1, 2, 3}) {
    MemNodeStore store;
    auto t = std::make_unique<BTree>(store, kSmall);
    std::map<TreeKey, Bytes> model;
    std::mt19937_64 rng(seed);
    unsigned tallest = 0;
    for (int step = 0; step < 20000; ++step) {
      std::uint64_t i = rng() % 3000;
      int op = static_cast<int>(rng() % 100);
      if (op < 50) {
        Bytes v = value(rng(), rng() % 3 == 0 ? rng() % 200 : rng() % 20);
        t->put(key(i), v);
        model[key(i)] = v;
      } else if (op < 85) {
        ASSERT_EQ(t->erase(key(i)), model.erase(key(i)) == 1) << "step " << step;
      } else if (op < 95) {
        auto got = t->get(key(i));
        auto it = model.find(key(i));
        ASSERT_EQ(got.has_value(), it != model.end()) << "step " << step;
        if (got) {
          ASSERT_EQ(*got, it->second);
        }
      } else if (op < 98) {
        // A range, in order.
        TreeKey from = key(rng() % 3000), to = key(rng() % 3000);
        if (to < from) std::swap(from, to);
        std::vector<TreeKey> got;
        t->scan(from, to, [&](const TreeKey& k, ByteView) {
          got.push_back(k);
          return true;
        });
        std::vector<TreeKey> want;
        for (auto it = model.lower_bound(from); it != model.end() && !(to < it->first); ++it) want.push_back(it->first);
        ASSERT_EQ(got, want) << "step " << step;
        auto lb = t->lower_bound(from);
        auto mlb = model.lower_bound(from);
        ASSERT_EQ(lb.has_value(), mlb != model.end());
        if (lb) {
          ASSERT_EQ(lb->first, mlb->first);
        }
      } else {
        // A checkpoint, and sometimes a fresh tree from the root (a mount).
        NodeRef root = t->flush();
        ASSERT_EQ(reachable(store, root), store.live) << "live nodes are exactly the tree's, step " << step;
        if (rng() % 2) t = std::make_unique<BTree>(store, kSmall, root);
      }
      tallest = std::max(tallest, t->height());
    }
    ASSERT_EQ(contents(*t), model);
    EXPECT_EQ(t->items(), model.size());
    EXPECT_GE(tallest, 4u) << "small nodes: several levels, so inner splits and merges happened";
    // Everything removed, through the levels.
    for (const auto& [k, v] : std::map<TreeKey, Bytes>(model)) ASSERT_TRUE(t->erase(k));
    EXPECT_FALSE(t->flush());
    EXPECT_TRUE(store.live.empty());
  }
}

TEST(BTree, OldRootsStayReadableAndOnlyReplacedNodesAreReleased) {
  MemNodeStore store;
  BTree t(store, kSmall);
  for (std::uint64_t i = 0; i < 2000; ++i) t.put(key(i), value(i, 30));
  NodeRef first = t.flush();
  std::set<std::uint64_t> before = reachable(store, first);
  std::uint64_t writes = store.writes;
  // One change: only its path is written again.
  t.put(key(1000), value(7, 30));
  NodeRef second = t.flush();
  EXPECT_EQ(store.writes - writes, t.height()) << "one node per level";
  std::set<std::uint64_t> after = reachable(store, second);
  std::size_t shared = 0;
  for (std::uint64_t o : after) shared += before.contains(o);
  EXPECT_EQ(shared, after.size() - t.height());
  // The old tree is still whole and unchanged (until its space is reused).
  BTree old(store, kSmall, first);
  EXPECT_EQ(*old.get(key(1000)), value(1000, 30));
  EXPECT_EQ(old.items(), 2000u);
}

TEST(BTree, LargeValuesAndSmallNodes) {
  MemNodeStore store;
  BTree t(store, kSmall);
  for (std::uint64_t i = 0; i < 50; ++i) t.put(key(i), value(i, i % 2 ? 3000 : 5));  // larger than a node
  NodeRef root = t.flush();
  BTree again(store, kSmall, root);
  for (std::uint64_t i = 0; i < 50; ++i) ASSERT_EQ(*again.get(key(i)), value(i, i % 2 ? 3000 : 5));
}

TEST(BTree, RelocateRewritesExactlyThatNode) {
  MemNodeStore store;
  BTree t(store, kSmall);
  for (std::uint64_t i = 0; i < 3000; ++i) t.put(key(i), value(i, 20));
  NodeRef root = t.flush();
  std::map<TreeKey, Bytes> want = contents(t);
  std::set<std::uint64_t> nodes = reachable(store, root);
  ASSERT_GT(nodes.size(), 20u);
  // Every node of the tree, one at a time, as GC would find them.
  std::size_t moved = 0;
  for (std::uint64_t o : nodes) {
    if (!store.live.contains(o)) continue;  // already moved with a child
    ASSERT_EQ(t.relocate(store.ref(o), store.info(o)), BTree::Relocated::moved) << o;
    ASSERT_EQ(t.relocate(store.ref(o), store.info(o)), BTree::Relocated::already_moving) << o;
    root = t.flush();
    ASSERT_FALSE(reachable(store, root).contains(o));
    ++moved;
  }
  EXPECT_GT(moved, 0u);
  EXPECT_EQ(contents(t), want);
  // A node no longer in the tree is not found.
  EXPECT_EQ(t.relocate(store.ref(*nodes.begin()), store.info(*nodes.begin())), BTree::Relocated::not_in_tree);
}

TEST(BTree, CacheStaysWithinBudget) {
  MemNodeStore store;
  BTree::Options o{.node_bytes = 4096, .cache_bytes = 64 << 10};
  NodeRef root;
  {
    BTree t(store, o);
    for (std::uint64_t i = 0; i < 20000; ++i) t.put(key(i), value(i, 40));
    root = t.flush();
  }
  BTree t(store, o, root);
  std::mt19937_64 rng(4);
  for (int i = 0; i < 5000; ++i) t.get(key(rng() % 20000));
  BTree::CacheStats s = t.cache_stats();
  EXPECT_LE(s.resident_bytes, (64 << 10) + 8192);
  EXPECT_GT(s.hits, 0u);
  EXPECT_GT(s.misses, 0u);
}

TEST(BTree, DamagedNodesAreReported) {
  MemNodeStore store;
  BTree t(store, kSmall);
  for (std::uint64_t i = 0; i < 500; ++i) t.put(key(i), value(i, 20));
  NodeRef root = t.flush();
  store.corrupt(root.offset);
  BTree again(store, kSmall, root);
  EXPECT_THROW(again.get(key(3)), Error);
  // The decoder refuses malformed nodes.
  Bytes junk = {0, 2};
  EXPECT_THROW(BTree::decode(junk), Error);
  Bytes unordered(1 + 1 + 34 + 2, 0);
  unordered[1] = 2;  // two keys, both zero: not increasing
  EXPECT_THROW(BTree::decode(unordered), Error);
  Bytes childless = {1, 0};
  EXPECT_THROW(BTree::decode(childless), Error);
}
