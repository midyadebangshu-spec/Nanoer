#include "nanoer/btree.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <unordered_map>

#include "nanoer/encoding.h"
#include "nanoer/error.h"

namespace nanoer {

// ---- nodes -----------------------------------------------------------------

struct BTree::Node {
  std::uint8_t level = 0;
  std::vector<TreeKey> keys;  // leaves: the items' keys; inner: each child's first key
  std::vector<Bytes> values;  // leaves
  struct Child {
    NodeRef ref;    // on disk (clean), or where the dirty copy came from
    NodePtr dirty;  // changed since the last flush
  };
  std::vector<Child> children;  // inner nodes
  NodeRef origin;               // dirty: the node on disk it replaces (released at flush)
  std::uint64_t bytes = 0;      // encoded size

  bool leaf() const { return level == 0; }
  std::size_t count() const { return keys.size(); }
};

namespace {

constexpr std::uint64_t kHeaderBytes = 12;  // level, count (generous)
constexpr std::uint64_t kKeyBytes = 17;
constexpr std::uint64_t kChildBytes = 16;   // offset and size as varints, at most

std::uint64_t varint_size(std::uint64_t v) {
  std::uint64_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

std::uint64_t item_bytes(ByteView value) { return kKeyBytes + varint_size(value.size()) + value.size(); }

void put_key(Encoder& out, const TreeKey& k) {
  std::uint8_t b[17];
  store_le(b, k.object);
  b[8] = k.kind;
  store_le(b + 9, k.offset);
  out.raw(ByteView(b, 17));
}

TreeKey get_key(Decoder& in) {
  ByteView b = in.raw(17);
  return {load_le<std::uint64_t>(b.data()), b[8], load_le<std::uint64_t>(b.data() + 9)};
}

[[noreturn]] void corrupt(const char* what) { throw Error(Errc::corrupt, std::string("tree node: ") + what); }

constexpr unsigned kMaxLevel = 32;
constexpr std::size_t kMaxValue = 16 << 20;

}  // namespace

// ---- the cache of clean nodes ------------------------------------------------

struct BTree::Cache {
  explicit Cache(std::uint64_t limit) : budget(limit) {}
  std::uint64_t budget;
  std::mutex mutex;
  std::list<std::pair<std::uint64_t, ConstNodePtr>> lru;  // most recent first
  std::unordered_map<std::uint64_t, decltype(lru)::iterator> index;
  std::uint64_t bytes = 0;
  std::uint64_t hits = 0, misses = 0;

  ConstNodePtr get(std::uint64_t offset) {
    std::lock_guard lock(mutex);
    auto it = index.find(offset);
    if (it == index.end()) {
      ++misses;
      return nullptr;
    }
    ++hits;
    lru.splice(lru.begin(), lru, it->second);
    return it->second->second;
  }
  void put(std::uint64_t offset, ConstNodePtr node) {
    std::lock_guard lock(mutex);
    erase_locked(offset);
    bytes += node->bytes;
    lru.emplace_front(offset, std::move(node));
    index.emplace(offset, lru.begin());
    while (bytes > budget && lru.size() > 1) {
      bytes -= lru.back().second->bytes;
      index.erase(lru.back().first);
      lru.pop_back();
    }
  }
  void erase(std::uint64_t offset) {
    std::lock_guard lock(mutex);
    erase_locked(offset);
  }
  void erase_locked(std::uint64_t offset) {
    auto it = index.find(offset);
    if (it == index.end()) return;
    bytes -= it->second->second->bytes;
    lru.erase(it->second);
    index.erase(it);
  }
};

// ---- encoding --------------------------------------------------------------
//
// level (u8), count (varint), the keys (17 bytes each: object, kind, offset,
// little-endian), then for a leaf each value (varint length, bytes), for an
// inner node each child (varint offset, varint stored size).

BTree::Decoded BTree::decode(ByteView encoded) {
  Decoder in(encoded);
  Decoded d;
  d.info.level = in.u8();
  if (d.info.level > kMaxLevel) corrupt("level out of range");
  std::uint64_t count = in.varint();
  if (count > in.remaining() / kKeyBytes) corrupt("count beyond its size");
  d.info.count = static_cast<std::uint32_t>(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    d.keys.push_back(get_key(in));
    if (i > 0 && !(d.keys[i - 1] < d.keys[i])) corrupt("keys out of order");
  }
  if (d.info.level == 0) {
    for (std::uint64_t i = 0; i < count; ++i) d.values.push_back(in.bytes(kMaxValue));
  } else {
    if (count == 0) corrupt("an inner node without children");
    for (std::uint64_t i = 0; i < count; ++i) {
      NodeRef r;
      r.offset = in.varint();
      std::uint64_t size = in.varint();
      if (r.offset == 0 || size == 0 || size > UINT32_MAX) corrupt("bad child location");
      r.stored_size = static_cast<std::uint32_t>(size);
      d.children.push_back(r);
    }
  }
  if (!in.done()) corrupt("trailing bytes");
  if (count) d.info.first = d.keys.front();
  return d;
}

// ---- the tree --------------------------------------------------------------

BTree::BTree(NodeStore& store, const Options& options, NodeRef root)
    : store_(store), options_(options), cache_(std::make_unique<Cache>(options.cache_bytes)), root_ref_(root) {}

BTree::~BTree() = default;

BTree::ConstNodePtr BTree::load(const NodeRef& ref) const {
  if (ConstNodePtr n = cache_->get(ref.offset)) return n;
  Decoded d = decode(store_.read(ref));
  auto node = std::make_shared<Node>();
  node->level = d.info.level;
  node->keys = std::move(d.keys);
  node->bytes = kHeaderBytes;
  if (node->leaf()) {
    node->values = std::move(d.values);
    for (const Bytes& v : node->values) node->bytes += item_bytes(v);
  } else {
    for (const NodeRef& c : d.children) node->children.push_back({c, nullptr});
    node->bytes += node->count() * (kKeyBytes + kChildBytes);
  }
  cache_->put(ref.offset, node);
  return node;
}

std::size_t BTree::child_index(const Node& node, const TreeKey& key) {
  auto it = std::upper_bound(node.keys.begin(), node.keys.end(), key);
  return it == node.keys.begin() ? 0 : static_cast<std::size_t>(it - node.keys.begin()) - 1;
}

BTree::NodePtr BTree::writable_root() {
  if (root_) return root_;
  if (root_ref_) {
    root_ = std::make_shared<Node>(*load(root_ref_));
    root_->origin = root_ref_;
  } else {
    root_ = std::make_shared<Node>();
    root_->bytes = kHeaderBytes;
  }
  dirty_bytes_ += options_.node_bytes;
  return root_;
}

BTree::NodePtr BTree::child_for_write(Node& parent, std::size_t i) {
  Node::Child& c = parent.children[i];
  if (c.dirty) return c.dirty;
  c.dirty = std::make_shared<Node>(*load(c.ref));
  c.dirty->origin = c.ref;
  dirty_bytes_ += options_.node_bytes;
  return c.dirty;
}

void BTree::release(const NodeRef& ref) {
  if (!ref) return;
  released_.push_back(ref);
}

void BTree::drop(Node& node) {
  release(node.origin);
  node.origin = {};
}

std::optional<Bytes> BTree::get(const TreeKey& key) const {
  const Node* node = root_.get();
  ConstNodePtr hold;
  if (!node) {
    if (!root_ref_) return std::nullopt;
    hold = load(root_ref_);
    node = hold.get();
  }
  while (!node->leaf()) {
    const Node::Child& c = node->children[child_index(*node, key)];
    if (c.dirty) {
      node = c.dirty.get();
    } else {
      hold = load(c.ref);
      node = hold.get();
    }
  }
  auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
  if (it == node->keys.end() || *it != key) return std::nullopt;
  return node->values[static_cast<std::size_t>(it - node->keys.begin())];
}

void BTree::put(const TreeKey& key, ByteView value) {
  if (value.size() > kMaxValue) throw Error(Errc::invalid_argument, "tree value too large");
  NodePtr root = writable_root();
  if (std::optional<Split> split = insert(*root, key, value)) {
    // The root split: a new root above both halves.
    auto top = std::make_shared<Node>();
    top->level = static_cast<std::uint8_t>(root->level + 1);
    top->keys = {root->keys.front(), split->first};
    top->children = {{root->origin, root}, {{}, split->right}};
    top->bytes = kHeaderBytes + 2 * (kKeyBytes + kChildBytes);
    // The old root's location is now the left child's origin.
    root_ = top;
    dirty_bytes_ += options_.node_bytes;
  }
}

std::optional<BTree::Split> BTree::insert(Node& node, const TreeKey& key, ByteView value) {
  if (node.leaf()) {
    auto it = std::lower_bound(node.keys.begin(), node.keys.end(), key);
    auto i = static_cast<std::size_t>(it - node.keys.begin());
    if (it != node.keys.end() && *it == key) {
      node.bytes -= item_bytes(node.values[i]);
      node.values[i].assign(value.begin(), value.end());
    } else {
      node.keys.insert(it, key);
      node.values.insert(node.values.begin() + static_cast<std::ptrdiff_t>(i), Bytes(value.begin(), value.end()));
    }
    node.bytes += item_bytes(value);
    if (node.bytes <= options_.node_bytes || node.count() < 2) return std::nullopt;
    // Split by size: the left half keeps at least half the bytes.
    std::uint64_t half = (node.bytes - kHeaderBytes) / 2, acc = 0;
    std::size_t at = 0;
    while (at + 1 < node.count() && acc + item_bytes(node.values[at]) <= half) acc += item_bytes(node.values[at++]);
    if (at == 0) at = 1;
    auto right = std::make_shared<Node>();
    right->level = 0;
    right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(at), node.keys.end());
    right->values.assign(std::make_move_iterator(node.values.begin() + static_cast<std::ptrdiff_t>(at)),
                         std::make_move_iterator(node.values.end()));
    node.keys.resize(at);
    node.values.resize(at);
    node.bytes = kHeaderBytes;
    for (const Bytes& v : node.values) node.bytes += item_bytes(v);
    right->bytes = kHeaderBytes;
    for (const Bytes& v : right->values) right->bytes += item_bytes(v);
    dirty_bytes_ += options_.node_bytes;
    return Split{right->keys.front(), right};
  }
  std::size_t i = child_index(node, key);
  NodePtr child = child_for_write(node, i);
  std::optional<Split> split = insert(*child, key, value);
  node.keys[i] = child->keys.front();
  if (!split) return std::nullopt;
  node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(i + 1), split->first);
  node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(i + 1), {{}, split->right});
  node.bytes += kKeyBytes + kChildBytes;
  if (node.bytes <= options_.node_bytes || node.count() < 4) return std::nullopt;
  std::size_t at = node.count() / 2;
  auto right = std::make_shared<Node>();
  right->level = node.level;
  right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(at), node.keys.end());
  right->children.assign(std::make_move_iterator(node.children.begin() + static_cast<std::ptrdiff_t>(at)),
                         std::make_move_iterator(node.children.end()));
  node.keys.resize(at);
  node.children.resize(at);
  node.bytes = kHeaderBytes + node.count() * (kKeyBytes + kChildBytes);
  right->bytes = kHeaderBytes + right->count() * (kKeyBytes + kChildBytes);
  dirty_bytes_ += options_.node_bytes;
  return Split{right->keys.front(), right};
}

bool BTree::erase(const TreeKey& key) {
  if (!root_ && !root_ref_) return false;
  if (!get(key)) return false;  // nothing to copy for a key that is not there
  NodePtr root = writable_root();
  remove(*root, key);
  // A root with one child is replaced by it; an empty leaf root is an
  // empty tree.
  while (!root_->leaf() && root_->count() <= 1) {
    if (root_->count() == 0) {  // everything removed: an empty leaf
      drop(*root_);
      root_ = std::make_shared<Node>();
      root_->bytes = kHeaderBytes;
      break;
    }
    NodePtr child = child_for_write(*root_, 0);
    drop(*root_);
    root_ = child;
  }
  return true;
}

bool BTree::remove(Node& node, const TreeKey& key) {
  if (node.leaf()) {
    auto it = std::lower_bound(node.keys.begin(), node.keys.end(), key);
    if (it == node.keys.end() || *it != key) return false;
    auto i = static_cast<std::size_t>(it - node.keys.begin());
    node.bytes -= item_bytes(node.values[i]);
    node.keys.erase(it);
    node.values.erase(node.values.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
  }
  std::size_t i = child_index(node, key);
  NodePtr child = child_for_write(node, i);
  bool removed = remove(*child, key);
  if (child->count() > 0) node.keys[i] = child->keys.front();
  if (child->count() == 0 || child->bytes < options_.node_bytes / 4) rebalance(node, i);
  return removed;
}

void BTree::rebalance(Node& parent, std::size_t i) {
  NodePtr child = parent.children[i].dirty;
  if (child->count() == 0) {
    drop(*child);
    parent.keys.erase(parent.keys.begin() + static_cast<std::ptrdiff_t>(i));
    parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(i));
    parent.bytes -= kKeyBytes + kChildBytes;
    dirty_bytes_ -= std::min<std::uint64_t>(dirty_bytes_, options_.node_bytes);
    return;
  }
  if (parent.count() < 2) return;
  // Merge with a neighbour if both fit in one node; else share evenly.
  std::size_t l = i > 0 ? i - 1 : i, r = l + 1;
  NodePtr left = child_for_write(parent, l), right = child_for_write(parent, r);
  std::uint64_t payload = left->bytes + right->bytes - kHeaderBytes;
  if (payload <= options_.node_bytes) {
    left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
    if (left->leaf()) {
      left->values.insert(left->values.end(), std::make_move_iterator(right->values.begin()),
                          std::make_move_iterator(right->values.end()));
    } else {
      left->children.insert(left->children.end(), std::make_move_iterator(right->children.begin()),
                            std::make_move_iterator(right->children.end()));
    }
    left->bytes = payload;
    drop(*right);
    parent.keys.erase(parent.keys.begin() + static_cast<std::ptrdiff_t>(r));
    parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(r));
    parent.bytes -= kKeyBytes + kChildBytes;
    dirty_bytes_ -= std::min<std::uint64_t>(dirty_bytes_, options_.node_bytes);
    return;
  }
  // Share: everything of both, split again near the middle by size.
  std::vector<TreeKey> keys = std::move(left->keys);
  keys.insert(keys.end(), right->keys.begin(), right->keys.end());
  std::size_t at;
  if (left->leaf()) {
    std::vector<Bytes> values = std::move(left->values);
    values.insert(values.end(), std::make_move_iterator(right->values.begin()), std::make_move_iterator(right->values.end()));
    std::uint64_t half = payload / 2, acc = 0;
    at = 0;
    while (at + 1 < keys.size() && acc + item_bytes(values[at]) <= half) acc += item_bytes(values[at++]);
    if (at == 0) at = 1;
    left->values.assign(std::make_move_iterator(values.begin()), std::make_move_iterator(values.begin() + static_cast<std::ptrdiff_t>(at)));
    right->values.assign(std::make_move_iterator(values.begin() + static_cast<std::ptrdiff_t>(at)), std::make_move_iterator(values.end()));
    left->bytes = right->bytes = kHeaderBytes;
    for (const Bytes& v : left->values) left->bytes += item_bytes(v);
    for (const Bytes& v : right->values) right->bytes += item_bytes(v);
  } else {
    std::vector<Node::Child> children = std::move(left->children);
    children.insert(children.end(), std::make_move_iterator(right->children.begin()), std::make_move_iterator(right->children.end()));
    at = keys.size() / 2;
    left->children.assign(std::make_move_iterator(children.begin()), std::make_move_iterator(children.begin() + static_cast<std::ptrdiff_t>(at)));
    right->children.assign(std::make_move_iterator(children.begin() + static_cast<std::ptrdiff_t>(at)), std::make_move_iterator(children.end()));
    left->bytes = kHeaderBytes + at * (kKeyBytes + kChildBytes);
    right->bytes = kHeaderBytes + (keys.size() - at) * (kKeyBytes + kChildBytes);
  }
  left->keys.assign(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(at));
  right->keys.assign(keys.begin() + static_cast<std::ptrdiff_t>(at), keys.end());
  parent.keys[l] = left->keys.front();
  parent.keys[r] = right->keys.front();
}

bool BTree::scan_node(const Node& node, const TreeKey& from, const TreeKey& to,
                      const std::function<bool(const TreeKey&, ByteView)>& fn) const {
  if (node.leaf()) {
    for (auto it = std::lower_bound(node.keys.begin(), node.keys.end(), from); it != node.keys.end(); ++it) {
      if (to < *it) return false;
      if (!fn(*it, node.values[static_cast<std::size_t>(it - node.keys.begin())])) return false;
    }
    return true;
  }
  for (std::size_t i = child_index(node, from); i < node.count(); ++i) {
    if (to < node.keys[i]) return false;
    const Node::Child& c = node.children[i];
    if (c.dirty) {
      if (!scan_node(*c.dirty, from, to, fn)) return false;
    } else {
      ConstNodePtr child = load(c.ref);
      if (!scan_node(*child, from, to, fn)) return false;
    }
  }
  return true;
}

void BTree::scan(const TreeKey& from, const TreeKey& to,
                 const std::function<bool(const TreeKey&, ByteView)>& fn) const {
  if (root_) {
    scan_node(*root_, from, to, fn);
  } else if (root_ref_) {
    ConstNodePtr root = load(root_ref_);
    scan_node(*root, from, to, fn);
  }
}

std::optional<std::pair<TreeKey, Bytes>> BTree::lower_bound(const TreeKey& key) const {
  std::optional<std::pair<TreeKey, Bytes>> out;
  scan(key, kMaxTreeKey, [&](const TreeKey& k, ByteView v) {
    out.emplace(k, Bytes(v.begin(), v.end()));
    return false;
  });
  return out;
}

NodeRef BTree::write(Node& node) {
  Encoder out;
  out.u8(node.level);
  out.varint(node.count());
  for (const TreeKey& k : node.keys) put_key(out, k);
  if (node.leaf()) {
    for (const Bytes& v : node.values) out.bytes(v);
  } else {
    for (Node::Child& c : node.children) {
      if (c.dirty) {
        c.ref = write(*c.dirty);
        c.dirty.reset();
      }
      out.varint(c.ref.offset);
      out.varint(c.ref.stored_size);
    }
  }
  NodeInfo info{node.level, node.keys.front(), static_cast<std::uint32_t>(node.count())};
  NodeRef ref = store_.write(out.buffer(), info);
  release(node.origin);
  node.origin = {};
  // Clean now: keep it for reads.
  cache_->put(ref.offset, std::make_shared<const Node>(std::move(node)));
  return ref;
}

NodeRef BTree::flush() {
  if (root_) {
    if (root_->count() == 0) {
      drop(*root_);
      root_ref_ = {};
    } else {
      root_ref_ = write(*root_);
    }
    root_.reset();
  }
  dirty_bytes_ = 0;
  // Only now: until the new root is written, the old nodes are the tree.
  for (const NodeRef& r : released_) {
    cache_->erase(r.offset);
    store_.release(r);
  }
  released_.clear();
  return root_ref_;
}

NodeRef BTree::root() const { return root_ref_; }
bool BTree::dirty() const { return root_ != nullptr; }

std::uint64_t BTree::count(const Node& node) const {
  if (node.leaf()) return node.count();
  std::uint64_t n = 0;
  for (const Node::Child& c : node.children) n += c.dirty ? count(*c.dirty) : count(*load(c.ref));
  return n;
}

std::uint64_t BTree::items() const {
  if (root_) return count(*root_);
  return root_ref_ ? count(*load(root_ref_)) : 0;
}

unsigned BTree::height() const {
  const Node* node = root_.get();
  ConstNodePtr hold;
  if (!node) {
    if (!root_ref_) return 0;
    hold = load(root_ref_);
    node = hold.get();
  }
  return node->level + 1u;
}

bool BTree::relocate(const NodeRef& ref, const NodeInfo& info) {
  // The root itself.
  if (root_ ? root_->origin == ref : root_ref_ == ref) {
    writable_root();
    return true;
  }
  if (!root_ && !root_ref_) return false;
  if ((root_ ? root_->level : load(root_ref_)->level) <= info.level) return false;
  // Down to the parent, copying the path as it goes only once the node is
  // found (a lookup first, read-only).
  std::vector<std::size_t> path;
  {
    const Node* node = root_.get();
    ConstNodePtr hold;
    if (!node) {
      hold = load(root_ref_);
      node = hold.get();
    }
    while (node->level > info.level + 1) {
      std::size_t i = child_index(*node, info.first);
      path.push_back(i);
      const Node::Child& c = node->children[i];
      if (c.dirty) {
        node = c.dirty.get();
      } else {
        hold = load(c.ref);
        node = hold.get();
      }
    }
    std::size_t i = child_index(*node, info.first);
    const Node::Child& c = node->children[i];
    if (c.dirty) return c.dirty->origin == ref;  // already being rewritten
    if (c.ref != ref) return false;
    path.push_back(i);
  }
  Node* node = writable_root().get();
  for (std::size_t i : path) node = child_for_write(*node, i).get();
  return true;
}

BTree::CacheStats BTree::cache_stats() const {
  std::lock_guard lock(cache_->mutex);
  return {cache_->bytes, cache_->hits, cache_->misses};
}

}  // namespace nanoer
