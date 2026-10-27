// Filesystem metadata on disk (M6): every inode, directory entry, chunk
// reference and extended attribute is an item in one copy-on-write B+tree
// (btree.h, docs/format.md §9), read through a cache, so memory no longer
// grows with the filesystem.
//
// Every change goes through apply(Op), as before M6: the filesystem applies
// operations as they happen and writes the same operations to the log, and
// mounting replays them with the same apply() on the tree as of the last
// checkpoint. A checkpoint writes the tree's changed nodes, then a small
// record with its root (encode_checkpoint).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nanoer/btree.h"
#include "nanoer/bytes.h"
#include "nanoer/dedup.h"
#include "nanoer/ops.h"

namespace nanoer {

class LegacyMetadata;

// Item kinds (docs/format.md §9). Part of the on-disk format.
namespace item_kind {
inline constexpr std::uint8_t inode = 1;   // object: inode; the attributes
inline constexpr std::uint8_t counts = 2;  // object: inode; entries in a directory, data bytes of a file
inline constexpr std::uint8_t xattr = 3;   // object: inode, offset: name hash; (name, value) pairs
inline constexpr std::uint8_t entry = 4;   // object: directory, offset: name hash; (name, inode) pairs
inline constexpr std::uint8_t chunk = 5;   // object: inode, offset: chunk index; the reference
inline constexpr std::uint8_t orphan = 6;  // object 0, offset: inode with no links (freed when unused)
inline constexpr std::uint8_t refs = 7;    // object: kRefsObject | record offset; count and users
}  // namespace item_kind
inline constexpr std::uint64_t kRefsObject = std::uint64_t{1} << 63;

// The 64-bit hash names are filed under (XXH3-64 of the name's bytes).
std::uint64_t name_hash(std::string_view name);

// Reads and writes nodes for the tree; Metadata does the space accounting.
class NodeIO {
 public:
  virtual ~NodeIO() = default;
  // The node at `ref`, whose segment has sequence number `seq`.
  virtual Bytes read(const NodeRef& ref, std::uint64_t seq) = 0;
  virtual NodeRef write(ByteView encoded, const NodeInfo& info) = 0;
};

// Nodes in memory: for tests, and for filesystems from before M6 mounted
// read-only (their metadata is converted in memory).
class MemNodeIO final : public NodeIO {
 public:
  Bytes read(const NodeRef& ref, std::uint64_t seq) override;
  NodeRef write(ByteView encoded, const NodeInfo& info) override;

 private:
  std::map<std::uint64_t, Bytes> nodes_;
  std::uint64_t next_ = 1;
};

struct MetaCounters {
  std::uint64_t node_bytes = 0;  // tree nodes on disk (part of the segments' live bytes)
  std::uint64_t files = 0;
  std::uint64_t directories = 0;
  std::uint64_t other_inodes = 0;
  std::uint64_t logical_bytes = 0;  // the regular files' sizes
  bool operator==(const MetaCounters&) const = default;
};

struct InodeCounts {
  std::uint64_t entries = 0;     // directories
  std::uint64_t data_bytes = 0;  // regular files: uncompressed bytes in chunks (holes excluded)
  bool operator==(const InodeCounts&) const = default;
};

class Metadata {
 public:
  struct Options {
    bool shared_records = true;
    bool cold_data = true;
    BTree::Options tree;
    // Nodes in memory: the segment table may not know them (MemNodeIO).
    bool account_nodes = true;
  };

  Metadata(std::uint64_t data_start, unsigned segment_shift, const Options& options,
           std::shared_ptr<NodeIO> io, NodeRef root = {});
  ~Metadata();
  Metadata(Metadata&&) noexcept;
  Metadata& operator=(Metadata&&) noexcept;

  // A new filesystem: just the root directory.
  static Metadata empty(std::uint64_t data_start, unsigned segment_shift, const Options& options,
                        std::shared_ptr<NodeIO> io, std::uint32_t uid, std::uint32_t gid,
                        std::int64_t now_ns);
  // Everything of a filesystem from before M6, as a tree (nanoer upgrade,
  // read-only mounts). Reference counts and back-references are rebuilt.
  static Metadata from_legacy(const LegacyMetadata& old, const Options& options, std::shared_ptr<NodeIO> io);
  // Everything, in memory (fsck, which checks the whole filesystem at once).
  // Every record with a refs item is listed in `shared`, with one
  // reference or more.
  LegacyMetadata to_legacy() const;
  // fsck of a tree that cannot be read whole (as of the last flush): what
  // is still readable. `problem` hears of each unreadable node (and of the
  // items left out); `node` sees every node named, readable or not.
  LegacyMetadata salvage(const std::function<void(const std::string&)>& problem,
                         const std::function<void(const NodeRef&)>& node) const;

  // Applies one change. Throws Error(corrupt) if it does not fit the current
  // state (during replay that means the log is damaged).
  void apply(const Op& op);

  // ---- reading (concurrent readers are safe while nothing changes) ----
  std::optional<InodeAttrs> attrs(InodeId ino) const;
  InodeCounts counts(InodeId ino) const;
  std::optional<ChunkRef> chunk(InodeId ino, std::uint64_t index) const;
  // Chunks with index >= `from`, in order, until `fn` returns false.
  void chunks(InodeId ino, std::uint64_t from, const std::function<bool(std::uint64_t, const ChunkRef&)>& fn) const;
  bool has_chunk_from(InodeId ino, std::uint64_t from) const;
  std::optional<InodeId> entry(InodeId dir, std::string_view name) const;
  // A directory's entries in name-hash order (stable positions).
  void entries(InodeId dir, const std::function<bool(std::string_view, InodeId)>& fn) const;
  std::optional<Bytes> xattr(InodeId ino, std::string_view name) const;
  void xattrs(InodeId ino, const std::function<bool(std::string_view, ByteView)>& fn) const;
  std::vector<InodeId> orphans() const;  // inodes with no links left
  // References to the record (1 unless it is a pack or shared).
  std::uint32_t references(const ChunkRef& ref) const;
  // Who refers to the record at `offset`: its back-references if it has
  // several (or is a pack); nothing if it has one (its header names it).
  std::optional<std::vector<ChunkUser>> users(std::uint64_t offset) const;
  // Walks of the whole filesystem (they read every node): background work,
  // statistics. Each stops when `fn` returns false.
  void for_each_inode(const std::function<bool(const InodeAttrs&)>& fn) const;
  void for_each_chunk(const std::function<bool(InodeId, std::uint64_t, const ChunkRef&)>& fn) const;
  // The same in steps: the chunks among at most `items` items from `from`.
  // Returns where the next step starts, or nothing at the end.
  std::optional<TreeKey> chunks_from(const TreeKey& from, std::size_t items,
                                     const std::function<void(InodeId, std::uint64_t, const ChunkRef&)>& fn) const;
  // Records with back-references (packs, shared records): only those items.
  void for_each_shared(const std::function<bool(std::uint64_t, const std::vector<ChunkUser>&)>& fn) const;
  // Every item in key order from `from`, until `fn` returns false.
  void scan(const TreeKey& from, const std::function<bool(const TreeKey&, ByteView)>& fn) const;

  std::uint64_t segment_of(std::uint64_t offset) const { return (offset - data_start_) >> segment_shift_; }
  bool shared_records() const { return options_.shared_records; }
  bool cold_data() const { return options_.cold_data; }

  // ---- checkpoints ----
  // Writes the tree's changed nodes (through the NodeIO).
  void flush();
  // The checkpoint record's contents (after flush()): root, segment table,
  // counters (docs/format.md §9).
  Bytes encode_checkpoint() const;
  static Metadata decode_checkpoint(ByteView data, std::uint64_t data_start, unsigned segment_shift,
                                    const Options& options, std::shared_ptr<NodeIO> io);
  static bool is_tree_checkpoint(ByteView stream_start);
  std::uint64_t dirty_bytes() const;  // changed since the last flush (roughly)

  const std::shared_ptr<NodeIO>& io() const { return io_; }
  BTree& tree() { return *tree_; }
  const BTree& tree() const { return *tree_; }

  // The same files, entries, chunks and attributes (for tests): compares
  // every item, the segment table and the counters.
  bool same_as(const Metadata& other) const;
  // What a legacy state would look like as items (for tests).
  bool same_as(const LegacyMetadata& legacy) const;

  // Every segment not free: holds log records, live data or nodes, or both.
  std::map<std::uint64_t, SegmentUsage> segments;
  MetaCounters counters;
  // Live data by content (dedup key -> record): a hint, within its budget,
  // rebuilt in the background after mount (dedup_complete is set then).
  DedupIndex dedup;
  bool dedup_complete = false;
  InodeId next_inode = kRootInode + 1;
  std::uint64_t last_txid = 0;  // the last transaction applied

 private:
  class Nodes;  // the tree's NodeStore: space accounting around the NodeIO

  void claim(std::uint64_t offset, std::uint64_t bytes);
  void release(std::uint64_t offset, std::uint64_t bytes);
  void add_ref(const ChunkRef& ref, bool share, const ChunkUser& user, const std::optional<ChunkUser>& owner);
  void drop_ref(const ChunkRef& ref, const ChunkUser& user);
  void count_inode(const InodeAttrs& a, int sign);
  void put_counts(InodeId ino, const InodeCounts& c);
  InodeAttrs need(InodeId ino) const;

  std::uint64_t data_start_;
  unsigned segment_shift_;
  Options options_;
  std::shared_ptr<NodeIO> io_;
  std::unique_ptr<Nodes> nodes_;
  std::unique_ptr<BTree> tree_;
};

}  // namespace nanoer
