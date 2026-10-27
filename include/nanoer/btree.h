// A copy-on-write B+tree (docs/M6_PLAN.md §3, KNOWLEDGE.md §7.5): sorted
// keys to byte values, with nodes stored elsewhere (a NodeStore) and read
// on demand through a cache.
//
// A change never touches a node that is on disk: it changes a copy in
// memory (a dirty node), and its parents up to the root are copied too.
// flush() writes the dirty nodes bottom-up and returns the new root; the
// nodes it replaced are released to the store, which keeps them readable
// until it is safe to reuse their space (for Nanoer, two checkpoints).
//
// Not thread-safe for changes; reads (get, scan) may run concurrently with
// each other once no change is in progress (the filesystem's shared lock).
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "nanoer/bytes.h"

namespace nanoer {

struct TreeKey {
  std::uint64_t object = 0;
  std::uint8_t kind = 0;
  std::uint64_t offset = 0;
  auto operator<=>(const TreeKey&) const = default;
};
inline constexpr TreeKey kMaxTreeKey{UINT64_MAX, UINT8_MAX, UINT64_MAX};

// Where a node is stored.
struct NodeRef {
  std::uint64_t offset = 0;  // 0: none
  std::uint32_t stored_size = 0;
  explicit operator bool() const { return offset != 0; }
  bool operator==(const NodeRef&) const = default;
};

// What a node is, as recorded with it: enough to find it again from the
// root (GC) and to check it is the node expected.
struct NodeInfo {
  std::uint8_t level = 0;  // 0: a leaf
  TreeKey first;           // its smallest key
  std::uint32_t count = 0;  // items or children
};

class NodeStore {
 public:
  virtual ~NodeStore() = default;
  // The node's encoding, as given to write(); throws Error(corrupt) if it
  // cannot be read.
  virtual Bytes read(const NodeRef& ref) = 0;
  virtual NodeRef write(ByteView encoded, const NodeInfo& info) = 0;
  // The tree no longer refers to the node (after a flush).
  virtual void release(const NodeRef& ref) = 0;
};

class BTree {
 public:
  struct Options {
    std::uint32_t node_bytes = 16 << 10;  // split above this (a leaf with one large item may exceed it)
    std::uint64_t cache_bytes = 64 << 20;  // clean nodes kept in memory
  };

  // An empty tree, or the one rooted at `root`.
  BTree(NodeStore& store, const Options& options, NodeRef root = {});
  ~BTree();
  BTree(const BTree&) = delete;
  BTree& operator=(const BTree&) = delete;

  std::optional<Bytes> get(const TreeKey& key) const;
  // Inserts or replaces.
  void put(const TreeKey& key, ByteView value);
  // Returns whether the key was there.
  bool erase(const TreeKey& key);
  // Calls `fn` for each item with from <= key <= to, in order, until it
  // returns false.
  void scan(const TreeKey& from, const TreeKey& to,
            const std::function<bool(const TreeKey&, ByteView)>& fn) const;
  // The first item with key >= `key`.
  std::optional<std::pair<TreeKey, Bytes>> lower_bound(const TreeKey& key) const;

  // Writes every dirty node and returns the root (none: the tree is empty).
  NodeRef flush();
  NodeRef root() const;  // as of the last flush (or construction)
  bool dirty() const;
  std::uint64_t dirty_bytes() const { return dirty_bytes_; }  // encoded size of the dirty nodes, roughly
  std::uint64_t items() const;  // walks the tree: for tests and fsck
  unsigned height() const;      // 1: the root is a leaf

  // GC: whether the node at `ref`, described by `info`, is part of the tree
  // (as of the last flush); if so, marks it and its parents dirty, so that
  // the next flush writes it elsewhere. Returns whether it was found.
  bool relocate(const NodeRef& ref, const NodeInfo& info);

  // The encoding of a node, for fsck and fuzzing: throws Error(corrupt).
  struct Decoded {
    NodeInfo info;
    std::vector<TreeKey> keys;
    std::vector<Bytes> values;     // leaves
    std::vector<NodeRef> children;  // inner nodes
  };
  static Decoded decode(ByteView encoded);

  struct CacheStats {
    std::uint64_t resident_bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
  };
  CacheStats cache_stats() const;

 private:
  struct Node;
  using NodePtr = std::shared_ptr<Node>;
  struct Split {
    TreeKey first;
    NodePtr right;
  };

  using ConstNodePtr = std::shared_ptr<const Node>;
  ConstNodePtr load(const NodeRef& ref) const;
  // The child to follow for `key` (the last whose first key is <= it).
  static std::size_t child_index(const Node& node, const TreeKey& key);
  void release(const NodeRef& ref);
  void drop(Node& node);  // a dirty node removed from the tree
  bool scan_node(const Node& node, const TreeKey& from, const TreeKey& to,
                 const std::function<bool(const TreeKey&, ByteView)>& fn) const;
  NodePtr child_for_write(Node& parent, std::size_t i);
  NodePtr writable_root();
  std::optional<Split> insert(Node& node, const TreeKey& key, ByteView value);
  bool remove(Node& node, const TreeKey& key);
  void rebalance(Node& parent, std::size_t i);
  NodeRef write(Node& node);
  std::uint64_t count(const Node& node) const;

  NodeStore& store_;
  Options options_;
  struct Cache;
  std::unique_ptr<Cache> cache_;  // clean nodes by location, decoded
  NodeRef root_ref_;          // on disk
  NodePtr root_;              // dirty, if the tree changed since the last flush
  std::vector<NodeRef> released_;  // replaced since the last flush
  std::uint64_t dirty_bytes_ = 0;
};

}  // namespace nanoer
