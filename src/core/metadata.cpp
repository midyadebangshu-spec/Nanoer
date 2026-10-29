#include "nanoer/metadata.h"

#include <algorithm>
#include <cstring>

#include <sys/stat.h>

#include "meta_encoding.h"
#include "nanoer/checksum.h"
#include "nanoer/encoding.h"
#include "nanoer/error.h"
#include "nanoer/legacy_metadata.h"

namespace nanoer {

using namespace meta;

namespace {

constexpr char kCheckpointMagicV3[8] = {'N', 'N', 'R', 'C', 'K', 'P', 'T', '3'};

DedupTarget target_of(const ChunkRef& ref) { return {ref.offset, ref.packed, ref.piece}; }

TreeKey key(InodeId object, std::uint8_t kind, std::uint64_t offset = 0) { return {object, kind, offset}; }
TreeKey refs_key(std::uint64_t record) { return {kRefsObject | record, item_kind::refs, 0}; }

bool is_dir(const InodeAttrs& a) { return S_ISDIR(a.mode); }
bool is_regular(const InodeAttrs& a) { return S_ISREG(a.mode); }

}  // namespace

// ---- items (docs/format.md §9.2), shared with fsck (meta_encoding.h) --------------

namespace meta {

TreeKey refs_key_of(std::uint64_t record) { return {kRefsObject | record, item_kind::refs, 0}; }

Bytes encode_inode(const InodeAttrs& a) {
  Encoder out;
  encode_attrs(out, a);
  return out.take();
}

InodeAttrs decode_inode(ByteView v) {
  Decoder in(v);
  InodeAttrs a = decode_attrs(in);
  if (!in.done()) corrupt("trailing bytes in an inode item");
  return a;
}

Bytes encode_entries(const std::vector<std::pair<std::string, InodeId>>& list) {
  Encoder out;
  out.varint(list.size());
  for (const auto& [name, ino] : list) {
    out.str(name);
    out.varint(ino);
  }
  return out.take();
}

std::vector<std::pair<std::string, InodeId>> decode_entries(ByteView v) {
  Decoder in(v);
  std::vector<std::pair<std::string, InodeId>> out;
  std::uint64_t n = in.varint();
  if (n == 0 || n > v.size()) corrupt("bad entry item");
  for (std::uint64_t i = 0; i < n; ++i) {
    std::string name = in.str(kMaxNameLength);
    out.emplace_back(std::move(name), in.varint());
  }
  if (!in.done()) corrupt("trailing bytes in an entry item");
  return out;
}

Bytes encode_xattrs(const std::vector<std::pair<std::string, Bytes>>& list) {
  Encoder out;
  out.varint(list.size());
  for (const auto& [name, value] : list) {
    out.str(name);
    out.bytes(value);
  }
  return out.take();
}

std::vector<std::pair<std::string, Bytes>> decode_xattrs(ByteView v) {
  Decoder in(v);
  std::vector<std::pair<std::string, Bytes>> out;
  std::uint64_t n = in.varint();
  if (n == 0 || n > v.size()) corrupt("bad xattr item");
  for (std::uint64_t i = 0; i < n; ++i) {
    std::string name = in.str(kMaxXattrNameLength);
    out.emplace_back(std::move(name), in.bytes(kMaxXattrValueSize));
  }
  if (!in.done()) corrupt("trailing bytes in an xattr item");
  return out;
}

Bytes encode_chunk_item(const ChunkRef& r) {
  Encoder out;
  encode_ref_ex(out, r, false);
  return out.take();
}

ChunkRef decode_chunk_item(ByteView v) {
  Decoder in(v);
  bool share = false;
  ChunkRef r = decode_ref_ex(in, share);
  if (share || !in.done()) corrupt("bad chunk item");
  if (!r.packed && r.piece) corrupt("a piece offset outside a pack");
  return r;
}

Bytes encode_refs(const Refs& r) {
  Encoder out;
  out.varint(r.count);
  out.varint(r.users.size());
  for (const ChunkUser& u : r.users) {
    out.varint(u.ino);
    out.varint(u.index);
  }
  return out.take();
}

Refs decode_refs(ByteView v) {
  Decoder in(v);
  Refs r;
  r.count = u32(in.varint());
  std::uint64_t n = in.varint();
  if (n != r.count || n > v.size()) corrupt("reference count disagrees with its users");
  for (std::uint64_t i = 0; i < n; ++i) {
    ChunkUser u;
    u.ino = in.varint();
    u.index = in.varint();
    r.users.push_back(u);
  }
  if (!in.done()) corrupt("trailing bytes in a refs item");
  return r;
}

Bytes encode_counts(const InodeCounts& c) {
  Encoder out;
  out.varint(c.entries);
  out.varint(c.data_bytes);
  return out.take();
}

InodeCounts decode_counts(ByteView v) {
  Decoder in(v);
  InodeCounts c;
  c.entries = in.varint();
  c.data_bytes = in.varint();
  if (!in.done()) corrupt("trailing bytes in a counts item");
  return c;
}

}  // namespace meta

std::uint64_t name_hash(std::string_view name) { return xxh3_64(as_bytes(name)); }

// ---- nodes -------------------------------------------------------------------

Bytes MemNodeIO::read(const NodeRef& ref, std::uint64_t) {
  auto it = nodes_.find(ref.offset);
  if (it == nodes_.end()) throw Error(Errc::corrupt, "no such tree node");
  return it->second;
}

NodeRef MemNodeIO::write(ByteView encoded, const NodeInfo&) {
  std::uint64_t at = next_++;
  nodes_[at].assign(encoded.begin(), encoded.end());
  return {at, static_cast<std::uint32_t>(encoded.size())};
}

class Metadata::Nodes final : public NodeStore {
 public:
  explicit Nodes(Metadata* meta) : owner(meta) {}
  Bytes read(const NodeRef& ref) override {
    std::uint64_t seq = 0;
    if (owner->options_.account_nodes) {
      auto it = owner->segments.find(owner->segment_of(ref.offset));
      if (it == owner->segments.end()) throw Error(Errc::corrupt, "tree node in a free segment");
      seq = it->second.seq;
    }
    return owner->io_->read(ref, seq);
  }
  NodeRef write(ByteView encoded, const NodeInfo& info) override {
    NodeRef ref = owner->io_->write(encoded, info);
    if (owner->options_.account_nodes) owner->claim(ref.offset, ref.stored_size);
    owner->counters.node_bytes += ref.stored_size;
    return ref;
  }
  void release(const NodeRef& ref) override {
    if (owner->options_.account_nodes) owner->release(ref.offset, ref.stored_size);
    owner->counters.node_bytes -= std::min<std::uint64_t>(owner->counters.node_bytes, ref.stored_size);
  }
  Metadata* owner;
};

// ---- construction --------------------------------------------------------------

Metadata::Metadata(std::uint64_t data_start, unsigned segment_shift, const Options& options,
                   std::shared_ptr<NodeIO> io, NodeRef root)
    : data_start_(data_start),
      segment_shift_(segment_shift),
      options_(options),
      io_(std::move(io)),
      nodes_(std::make_unique<Nodes>(this)),
      tree_(std::make_unique<BTree>(*nodes_, options.tree, root)) {
  options_.cold_data = options_.shared_records && options_.cold_data;
}

Metadata::~Metadata() = default;

Metadata::Metadata(Metadata&& o) noexcept
    : segments(std::move(o.segments)),
      counters(o.counters),
      dedup(std::move(o.dedup)),
      dedup_complete(o.dedup_complete),
      next_inode(o.next_inode),
      last_txid(o.last_txid),
      data_start_(o.data_start_),
      segment_shift_(o.segment_shift_),
      options_(o.options_),
      io_(std::move(o.io_)),
      nodes_(std::move(o.nodes_)),
      tree_(std::move(o.tree_)) {
  if (nodes_) nodes_->owner = this;
}

Metadata& Metadata::operator=(Metadata&& o) noexcept {
  segments = std::move(o.segments);
  counters = o.counters;
  dedup = std::move(o.dedup);
  dedup_complete = o.dedup_complete;
  next_inode = o.next_inode;
  last_txid = o.last_txid;
  data_start_ = o.data_start_;
  segment_shift_ = o.segment_shift_;
  options_ = o.options_;
  io_ = std::move(o.io_);
  tree_.reset();
  nodes_ = std::move(o.nodes_);
  tree_ = std::move(o.tree_);
  if (nodes_) nodes_->owner = this;
  return *this;
}

Metadata Metadata::empty(std::uint64_t data_start, unsigned segment_shift, const Options& options,
                         std::shared_ptr<NodeIO> io, std::uint32_t uid, std::uint32_t gid, std::int64_t now_ns) {
  Metadata m(data_start, segment_shift, options, std::move(io));
  InodeAttrs root;
  root.id = kRootInode;
  root.mode = S_IFDIR | 0755;
  root.uid = uid;
  root.gid = gid;
  root.nlink = 2;
  root.atime_ns = root.mtime_ns = root.ctime_ns = root.btime_ns = now_ns;
  root.parent = kRootInode;
  m.apply(op::PutInode{root});
  m.dedup_complete = true;
  return m;
}

// ---- reading -------------------------------------------------------------------

std::optional<InodeAttrs> Metadata::attrs(InodeId ino) const {
  std::optional<Bytes> v = tree_->get(key(ino, item_kind::inode));
  if (!v) return std::nullopt;
  return decode_inode(*v);
}

InodeAttrs Metadata::need(InodeId ino) const {
  std::optional<InodeAttrs> a = attrs(ino);
  if (!a) corrupt("operation on missing inode " + std::to_string(ino));
  return *a;
}

InodeCounts Metadata::counts(InodeId ino) const {
  std::optional<Bytes> v = tree_->get(key(ino, item_kind::counts));
  return v ? decode_counts(*v) : InodeCounts{};
}

void Metadata::put_counts(InodeId ino, const InodeCounts& c) {
  if (c == InodeCounts{}) {
    tree_->erase(key(ino, item_kind::counts));
  } else {
    tree_->put(key(ino, item_kind::counts), encode_counts(c));
  }
}

std::optional<ChunkRef> Metadata::chunk(InodeId ino, std::uint64_t index) const {
  std::optional<Bytes> v = tree_->get(key(ino, item_kind::chunk, index));
  if (!v) return std::nullopt;
  return decode_chunk_item(*v);
}

void Metadata::chunks(InodeId ino, std::uint64_t from,
                      const std::function<bool(std::uint64_t, const ChunkRef&)>& fn) const {
  tree_->scan(key(ino, item_kind::chunk, from), key(ino, item_kind::chunk, UINT64_MAX),
              [&](const TreeKey& k, ByteView v) { return fn(k.offset, decode_chunk_item(v)); });
}

bool Metadata::has_chunk_from(InodeId ino, std::uint64_t from) const {
  bool any = false;
  tree_->scan(key(ino, item_kind::chunk, from), key(ino, item_kind::chunk, UINT64_MAX), [&](const TreeKey&, ByteView) {
    any = true;
    return false;
  });
  return any;
}

std::optional<InodeId> Metadata::entry(InodeId dir, std::string_view name) const {
  std::optional<Bytes> v = tree_->get(key(dir, item_kind::entry, name_hash(name)));
  if (!v) return std::nullopt;
  for (const auto& [n, ino] : decode_entries(*v)) {
    if (n == name) return ino;
  }
  return std::nullopt;
}

void Metadata::entries(InodeId dir, const std::function<bool(std::string_view, InodeId)>& fn) const {
  tree_->scan(key(dir, item_kind::entry, 0), key(dir, item_kind::entry, UINT64_MAX), [&](const TreeKey&, ByteView v) {
    for (const auto& [n, ino] : decode_entries(v)) {
      if (!fn(n, ino)) return false;
    }
    return true;
  });
}

std::optional<Bytes> Metadata::xattr(InodeId ino, std::string_view name) const {
  std::optional<Bytes> v = tree_->get(key(ino, item_kind::xattr, name_hash(name)));
  if (!v) return std::nullopt;
  for (auto& [n, value] : decode_xattrs(*v)) {
    if (n == name) return std::move(value);
  }
  return std::nullopt;
}

void Metadata::xattrs(InodeId ino, const std::function<bool(std::string_view, ByteView)>& fn) const {
  tree_->scan(key(ino, item_kind::xattr, 0), key(ino, item_kind::xattr, UINT64_MAX), [&](const TreeKey&, ByteView v) {
    for (const auto& [n, value] : decode_xattrs(v)) {
      if (!fn(n, value)) return false;
    }
    return true;
  });
}

std::vector<InodeId> Metadata::orphans() const {
  std::vector<InodeId> out;
  tree_->scan(key(0, item_kind::orphan, 0), key(0, item_kind::orphan, UINT64_MAX), [&](const TreeKey& k, ByteView) {
    out.push_back(k.offset);
    return true;
  });
  return out;
}

std::uint32_t Metadata::references(const ChunkRef& ref) const {
  std::optional<Bytes> v = tree_->get(refs_key(ref.offset));
  return v ? decode_refs(*v).count : 1;
}

std::optional<std::vector<ChunkUser>> Metadata::users(std::uint64_t offset) const {
  std::optional<Bytes> v = tree_->get(refs_key(offset));
  if (!v) return std::nullopt;
  return decode_refs(*v).users;
}

void Metadata::for_each_inode(const std::function<bool(const InodeAttrs&)>& fn) const {
  tree_->scan({}, {kRefsObject - 1, UINT8_MAX, UINT64_MAX}, [&](const TreeKey& k, ByteView v) {
    return k.kind != item_kind::inode || fn(decode_inode(v));
  });
}

void Metadata::for_each_chunk(const std::function<bool(InodeId, std::uint64_t, const ChunkRef&)>& fn) const {
  tree_->scan({}, {kRefsObject - 1, UINT8_MAX, UINT64_MAX}, [&](const TreeKey& k, ByteView v) {
    return k.kind != item_kind::chunk || fn(k.object, k.offset, decode_chunk_item(v));
  });
}

std::optional<TreeKey> Metadata::chunks_from(
    const TreeKey& from, std::size_t items,
    const std::function<void(InodeId, std::uint64_t, const ChunkRef&)>& fn) const {
  std::optional<TreeKey> next;
  std::size_t seen = 0;
  tree_->scan(from, kMaxTreeKey, [&](const TreeKey& k, ByteView v) {
    if (seen++ == items) {
      next = k;
      return false;
    }
    if (k.kind == item_kind::chunk && !(k.object & kRefsObject)) fn(k.object, k.offset, decode_chunk_item(v));
    return true;
  });
  return next;
}

void Metadata::for_each_shared(const std::function<bool(std::uint64_t, const std::vector<ChunkUser>&)>& fn) const {
  tree_->scan({kRefsObject, 0, 0}, kMaxTreeKey, [&](const TreeKey& k, ByteView v) {
    return k.kind != item_kind::refs || fn(k.object & ~kRefsObject, decode_refs(v).users);
  });
}

void Metadata::scan(const TreeKey& from, const std::function<bool(const TreeKey&, ByteView)>& fn) const {
  tree_->scan(from, kMaxTreeKey, fn);
}

// ---- space and references -------------------------------------------------------

void Metadata::claim(std::uint64_t offset, std::uint64_t bytes) {
  if (offset < data_start_) corrupt("location before the data area");
  auto it = segments.find(segment_of(offset));
  if (it == segments.end()) corrupt("chunk in a segment that is not in use");
  it->second.live_bytes += bytes;
}

void Metadata::release(std::uint64_t offset, std::uint64_t bytes) {
  auto it = segments.find(segment_of(offset));
  if (it == segments.end() || it->second.live_bytes < bytes) corrupt("segment space accounting is inconsistent");
  it->second.live_bytes -= bytes;
}

// A record's space is claimed by its first reference and released by its
// last. Packs, and records that have been shared, have a refs item with
// every reference (back-references); any other record has one reference,
// the owner its header names.
void Metadata::add_ref(const ChunkRef& ref, bool share, const ChunkUser& user, const std::optional<ChunkUser>& owner) {
  if ((ref.packed || share || ref.hash) && !options_.shared_records) {
    corrupt("shared records on a filesystem without that feature");
  }
  if (ref.cold && !options_.cold_data) corrupt("cold data on a filesystem without that feature");
  if (ref.hash) dedup.put(ref.hash, target_of(ref));
  std::optional<Bytes> v = tree_->get(refs_key(ref.offset));
  if (ref.packed) {
    if (share) corrupt("a pack piece marked as shared");
    Refs r = v ? decode_refs(*v) : Refs{};
    if (!v) claim(ref.offset, ref.stored_size);
    if (r.count == UINT32_MAX) corrupt("too many references to one record");
    ++r.count;
    r.users.push_back(user);
    tree_->put(refs_key(ref.offset), encode_refs(r));
  } else if (share) {
    Refs r;
    if (v) {
      r = decode_refs(*v);
    } else {
      if (!owner) corrupt("a record shared for the first time without its owner");
      r = {1, {*owner}};
    }
    if (r.count == UINT32_MAX) corrupt("too many references to one record");
    ++r.count;
    r.users.push_back(user);
    tree_->put(refs_key(ref.offset), encode_refs(r));
  } else {
    if (v) corrupt("a new chunk at a location already in use");
    claim(ref.offset, ref.stored_size);
  }
}

void Metadata::drop_ref(const ChunkRef& ref, const ChunkUser& user) {
  // Pieces are not counted one by one in the index: a piece leaves it with
  // any reference to it (a missed duplicate at worst); other data with its
  // record.
  if (ref.hash && ref.packed) dedup.erase(ref.hash, target_of(ref));
  std::optional<Bytes> v = tree_->get(refs_key(ref.offset));
  if (!v) {
    if (ref.packed) corrupt("a pack piece of an unknown pack");
    if (ref.hash) dedup.erase(ref.hash, target_of(ref));
    release(ref.offset, ref.stored_size);
    return;
  }
  Refs r = decode_refs(*v);
  auto it = std::find(r.users.begin(), r.users.end(), user);
  if (it == r.users.end()) corrupt("a reference missing from its record's users");
  r.users.erase(it);
  --r.count;
  if (r.count > 0) {
    tree_->put(refs_key(ref.offset), encode_refs(r));
    return;
  }
  tree_->erase(refs_key(ref.offset));
  if (ref.hash && !ref.packed) dedup.erase(ref.hash, target_of(ref));
  release(ref.offset, ref.stored_size);
}

void Metadata::count_inode(const InodeAttrs& a, int sign) {
  auto add = [&](std::uint64_t& c, std::uint64_t n) { c = sign > 0 ? c + n : c - std::min(c, n); };
  if (is_regular(a)) {
    add(counters.files, 1);
    add(counters.logical_bytes, a.size);
  } else if (is_dir(a)) {
    add(counters.directories, 1);
  } else {
    add(counters.other_inodes, 1);
  }
}

// ---- operations ------------------------------------------------------------------

void Metadata::apply(const Op& op) {
  auto need_dir = [&](InodeId ino) {
    InodeAttrs a = need(ino);
    if (!is_dir(a)) corrupt("directory operation on non-directory " + std::to_string(ino));
    return a;
  };
  auto need_file = [&](InodeId ino) {
    InodeAttrs a = need(ino);
    if (!is_regular(a)) corrupt("chunk operation on non-file " + std::to_string(ino));
    return a;
  };
  auto set_chunk = [&](InodeId ino, std::uint64_t index, const ChunkRef& ref, bool share,
                       const std::optional<ChunkUser>& owner) {
    need_file(ino);
    std::optional<ChunkRef> old = chunk(ino, index);
    ChunkUser user{ino, index};
    add_ref(ref, share, user, owner);
    if (old) drop_ref(*old, user);
    tree_->put(key(ino, item_kind::chunk, index), encode_chunk_item(ref));
    InodeCounts c = counts(ino);
    c.data_bytes = c.data_bytes - std::min<std::uint64_t>(c.data_bytes, old ? old->size : 0) + ref.size;
    put_counts(ino, c);
  };
  auto remove_chunks = [&](InodeId ino, std::uint64_t from, std::uint64_t to) {
    std::vector<std::pair<std::uint64_t, ChunkRef>> gone;
    chunks(ino, from, [&](std::uint64_t index, const ChunkRef& ref) {
      if (index > to) return false;
      gone.emplace_back(index, ref);
      return true;
    });
    if (gone.empty()) return;
    InodeCounts c = counts(ino);
    for (const auto& [index, ref] : gone) {
      drop_ref(ref, {ino, index});
      tree_->erase(key(ino, item_kind::chunk, index));
      c.data_bytes -= std::min<std::uint64_t>(c.data_bytes, ref.size);
    }
    put_counts(ino, c);
  };

  std::visit(
      Overloaded{
          [&](const op::PutInode& o) {
            if (o.attrs.id == 0) corrupt("inode number 0");
            if (std::optional<InodeAttrs> old = attrs(o.attrs.id)) count_inode(*old, -1);
            count_inode(o.attrs, +1);
            tree_->put(key(o.attrs.id, item_kind::inode), encode_inode(o.attrs));
            if (o.attrs.nlink == 0 && o.attrs.id != kRootInode) {
              tree_->put(key(0, item_kind::orphan, o.attrs.id), {});
            } else {
              tree_->erase(key(0, item_kind::orphan, o.attrs.id));
            }
            next_inode = std::max(next_inode, o.attrs.id + 1);
          },
          [&](const op::DeleteInode& o) {
            if (o.ino == kRootInode) corrupt("deleting the root directory");
            InodeAttrs a = need(o.ino);
            remove_chunks(o.ino, 0, UINT64_MAX);
            // Everything else under the inode: attributes, counts, xattrs, entries.
            std::vector<TreeKey> keys;
            tree_->scan(key(o.ino, 0, 0), key(o.ino, item_kind::entry, UINT64_MAX), [&](const TreeKey& k, ByteView) {
              keys.push_back(k);
              return true;
            });
            for (const TreeKey& k : keys) tree_->erase(k);
            tree_->erase(key(0, item_kind::orphan, o.ino));
            count_inode(a, -1);
          },
          [&](const op::SetEntry& o) {
            if (!valid_name(o.name)) corrupt("invalid file name");
            need(o.ino);
            need_dir(o.dir);
            TreeKey k = key(o.dir, item_kind::entry, name_hash(o.name));
            std::optional<Bytes> v = tree_->get(k);
            auto list = v ? decode_entries(*v) : std::vector<std::pair<std::string, InodeId>>{};
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == o.name; });
            if (it != list.end()) {
              it->second = o.ino;
            } else {
              list.emplace_back(o.name, o.ino);
              InodeCounts c = counts(o.dir);
              ++c.entries;
              put_counts(o.dir, c);
            }
            tree_->put(k, encode_entries(list));
          },
          [&](const op::RemoveEntry& o) {
            need_dir(o.dir);
            TreeKey k = key(o.dir, item_kind::entry, name_hash(o.name));
            std::optional<Bytes> v = tree_->get(k);
            auto list = v ? decode_entries(*v) : std::vector<std::pair<std::string, InodeId>>{};
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == o.name; });
            if (it == list.end()) corrupt("removing a missing directory entry");
            list.erase(it);
            if (list.empty()) {
              tree_->erase(k);
            } else {
              tree_->put(k, encode_entries(list));
            }
            InodeCounts c = counts(o.dir);
            c.entries -= std::min<std::uint64_t>(c.entries, 1);
            put_counts(o.dir, c);
          },
          [&](const op::SetChunk& o) {
            if (o.ref.packed || o.ref.piece || o.ref.hash || o.ref.cold) corrupt("extended chunk fields in SetChunk");
            set_chunk(o.ino, o.index, o.ref, false, std::nullopt);
          },
          [&](const op::SetChunkRef& o) {
            if (!o.ref.packed && o.ref.piece) corrupt("a piece offset outside a pack");
            set_chunk(o.ino, o.index, o.ref, o.share, o.owner);
          },
          [&](const op::RemoveChunk& o) {
            need_file(o.ino);
            remove_chunks(o.ino, o.index, o.index);
          },
          [&](const op::TruncateChunks& o) {
            need_file(o.ino);
            remove_chunks(o.ino, o.first_index, UINT64_MAX);
          },
          [&](const op::SetXattr& o) {
            need(o.ino);
            TreeKey k = key(o.ino, item_kind::xattr, name_hash(o.name));
            std::optional<Bytes> v = tree_->get(k);
            auto list = v ? decode_xattrs(*v) : std::vector<std::pair<std::string, Bytes>>{};
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == o.name; });
            if (it != list.end()) {
              it->second = o.value;
            } else {
              list.emplace_back(o.name, o.value);
            }
            tree_->put(k, encode_xattrs(list));
          },
          [&](const op::RemoveXattr& o) {
            need(o.ino);
            TreeKey k = key(o.ino, item_kind::xattr, name_hash(o.name));
            std::optional<Bytes> v = tree_->get(k);
            auto list = v ? decode_xattrs(*v) : std::vector<std::pair<std::string, Bytes>>{};
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == o.name; });
            if (it == list.end()) corrupt("removing a missing xattr");
            list.erase(it);
            if (list.empty()) {
              tree_->erase(k);
            } else {
              tree_->put(k, encode_xattrs(list));
            }
          },
      },
      op);
}

// ---- checkpoints -------------------------------------------------------------------

void Metadata::flush() { tree_->flush(); }

std::uint64_t Metadata::dirty_bytes() const { return tree_->dirty_bytes(); }

bool Metadata::is_tree_checkpoint(ByteView stream_start) {
  return stream_start.size() >= 8 && std::memcmp(stream_start.data(), kCheckpointMagicV3, 8) == 0;
}

Bytes Metadata::encode_checkpoint() const {
  if (tree_->dirty()) throw Error(Errc::invalid_argument, "checkpoint of a tree not flushed");
  Encoder out;
  out.raw(ByteView(reinterpret_cast<const std::uint8_t*>(kCheckpointMagicV3), 8));
  out.varint(last_txid);
  out.varint(next_inode);
  NodeRef root = tree_->root();
  out.varint(root.offset);
  out.varint(root.stored_size);
  out.varint(counters.node_bytes);
  out.varint(counters.files);
  out.varint(counters.directories);
  out.varint(counters.other_inodes);
  out.varint(counters.logical_bytes);
  out.varint(segments.size());
  for (const auto& [index, usage] : segments) {
    out.varint(index);
    out.varint(usage.seq);
    out.varint(usage.live_bytes);
  }
  return out.take();
}

Metadata Metadata::decode_checkpoint(ByteView data, std::uint64_t data_start, unsigned segment_shift,
                                     const Options& options, std::shared_ptr<NodeIO> io) {
  Decoder in(data);
  if (std::memcmp(in.raw(8).data(), kCheckpointMagicV3, 8) != 0) corrupt("not a tree checkpoint");
  std::uint64_t txid = in.varint();
  std::uint64_t next = in.varint();
  NodeRef root;
  root.offset = in.varint();
  root.stored_size = u32(in.varint());
  if ((root.offset == 0) != (root.stored_size == 0)) corrupt("bad tree root");
  Metadata m(data_start, segment_shift, options, std::move(io), root);
  m.last_txid = txid;
  m.next_inode = next;
  m.counters.node_bytes = in.varint();
  m.counters.files = in.varint();
  m.counters.directories = in.varint();
  m.counters.other_inodes = in.varint();
  m.counters.logical_bytes = in.varint();
  std::uint64_t n = in.varint();
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t index = in.varint();
    std::uint64_t seq = in.varint();
    std::uint64_t live = in.varint();
    if (!m.segments.emplace(index, SegmentUsage{seq, live}).second) corrupt("duplicate segment");
  }
  if (!in.done()) corrupt("trailing bytes in checkpoint");
  if (root && options.account_nodes) {  // (nodes in memory are not in segments)
    if (root.offset < data_start) corrupt("tree root before the data area");
    if (!m.segments.contains(m.segment_of(root.offset))) corrupt("tree root in a free segment");
  }
  return m;
}

// ---- legacy filesystems ----------------------------------------------------------------

Metadata Metadata::from_legacy(const LegacyMetadata& old, const Options& options, std::shared_ptr<NodeIO> io) {
  Options o = options;
  o.shared_records = old.shared_records();
  o.cold_data = old.cold_data();
  Metadata m(old.data_start(), old.segment_shift(), o, std::move(io));
  m.segments = old.segments;
  m.next_inode = old.next_inode;
  m.last_txid = old.last_txid;
  m.dedup = old.dedup;
  m.dedup_complete = true;
  std::vector<InodeId> ids;
  ids.reserve(old.inodes.size());
  for (const auto& [id, inode] : old.inodes) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  std::map<std::uint64_t, Refs> refs;
  for (InodeId id : ids) {
    const Inode& inode = old.inodes.at(id);
    const InodeAttrs& a = inode;
    m.tree_->put(key(id, item_kind::inode), encode_inode(a));
    m.count_inode(a, +1);
    if (a.nlink == 0 && id != kRootInode) m.tree_->put(key(0, item_kind::orphan, id), {});
    InodeCounts c;
    c.entries = inode.entries.size();
    std::map<std::uint64_t, std::vector<std::pair<std::string, InodeId>>> entries;
    for (const auto& [name, child] : inode.entries) entries[name_hash(name)].emplace_back(name, child);
    for (const auto& [h, list] : entries) m.tree_->put(key(id, item_kind::entry, h), encode_entries(list));
    std::map<std::uint64_t, std::vector<std::pair<std::string, Bytes>>> xattrs;
    for (const auto& [name, value] : inode.xattrs) xattrs[name_hash(name)].emplace_back(name, value);
    for (const auto& [h, list] : xattrs) m.tree_->put(key(id, item_kind::xattr, h), encode_xattrs(list));
    for (const auto& [index, ref] : inode.chunks) {
      c.data_bytes += ref.size;
      m.tree_->put(key(id, item_kind::chunk, index), encode_chunk_item(ref));
      if (ref.packed || old.shared.contains(ref.offset)) {
        Refs& r = refs[ref.offset];
        ++r.count;
        r.users.push_back({id, index});
      }
    }
    m.put_counts(id, c);
  }
  for (const auto& [offset, r] : refs) m.tree_->put(refs_key(offset), encode_refs(r));
  return m;
}

namespace {

// Builds the in-memory form from the items, in key order.
struct LegacyBuilder {
  LegacyMetadata& m;
  Inode* inode = nullptr;

  bool add(const TreeKey& k, ByteView v) {
    if (k.kind == item_kind::refs) {
      if (!(k.object & kRefsObject) || k.offset != 0) corrupt("misplaced refs item");
      m.shared[k.object & ~kRefsObject] = decode_refs(v).count;
      return true;
    }
    if (k.kind == item_kind::orphan) {
      if (k.object != 0) corrupt("misplaced orphan item");
      return true;
    }
    if (k.kind == item_kind::inode) {
      if (k.offset != 0) corrupt("misplaced inode item");
      InodeAttrs a = decode_inode(v);
      if (a.id != k.object) corrupt("inode item under another inode number");
      Inode& i = m.inodes[k.object];
      static_cast<InodeAttrs&>(i) = a;
      inode = &i;
      return true;
    }
    // The inode item comes first: every other item of an inode follows it.
    if (!inode || inode->id != k.object) corrupt("an item of an inode that does not exist");
    switch (k.kind) {
      case item_kind::counts:
        decode_counts(v);
        break;
      case item_kind::xattr:
        for (auto& [name, value] : decode_xattrs(v)) {
          if (name_hash(name) != k.offset) corrupt("xattr item under another name's hash");
          if (!inode->xattrs.emplace(std::move(name), std::move(value)).second) corrupt("duplicate xattr");
        }
        break;
      case item_kind::entry:
        for (auto& [name, child] : decode_entries(v)) {
          if (name_hash(name) != k.offset) corrupt("entry item under another name's hash");
          if (!inode->entries.emplace(std::move(name), child).second) corrupt("duplicate entry");
        }
        break;
      case item_kind::chunk: {
        ChunkRef ref = decode_chunk_item(v);
        if (ref.hash) m.dedup.put(ref.hash, target_of(ref));
        inode->chunks.emplace(k.offset, ref);
        break;
      }
      default:
        corrupt("an item of unknown kind");
    }
    return true;
  }
};

}  // namespace

LegacyMetadata Metadata::to_legacy() const {
  LegacyMetadata m(data_start_, segment_shift_, options_.shared_records, options_.cold_data);
  m.segments = segments;
  m.next_inode = next_inode;
  m.last_txid = last_txid;
  LegacyBuilder b{m};
  tree_->scan({}, kMaxTreeKey, [&](const TreeKey& k, ByteView v) { return b.add(k, v); });
  return m;
}

LegacyMetadata Metadata::salvage(const std::function<void(const std::string&)>& problem,
                                 const std::function<void(const NodeRef&)>& node) const {
  LegacyMetadata m(data_start_, segment_shift_, options_.shared_records, options_.cold_data);
  m.segments = segments;
  m.next_inode = next_inode;
  m.last_txid = last_txid;
  LegacyBuilder b{m};
  std::uint64_t skipped = 0;
  tree_->salvage(
      [&](const TreeKey& k, ByteView v) {
        try {
          b.add(k, v);
        } catch (const Error& e) {
          if (e.code() != Errc::corrupt) throw;
          ++skipped;
        }
      },
      node, [&](const NodeRef&, const std::string& what) { problem(what); });
  if (skipped) problem(std::to_string(skipped) + " items that cannot be used (damaged, or of an inode lost with its node)");
  return m;
}

namespace {

// `skip_single`: leave out the users of shared records down to one
// reference, which the legacy metadata does not keep.
bool same_items(const BTree& a, const BTree& b, bool skip_single = false) {
  std::vector<std::pair<TreeKey, Bytes>> x, y;
  auto collect = [&](const BTree& t, std::vector<std::pair<TreeKey, Bytes>>& out) {
    t.scan({}, kMaxTreeKey, [&](const TreeKey& k, ByteView v) {
      if (skip_single && k.kind == item_kind::refs) {
        Refs r = decode_refs(v);
        if (r.count == 1) {
          std::optional<Bytes> c = t.get({r.users[0].ino, item_kind::chunk, r.users[0].index});
          if (c && !decode_chunk_item(*c).packed) return true;
        }
      }
      if (k.kind == item_kind::refs) {
        // Users in any order: the order they came in.
        Refs r = decode_refs(v);
        std::sort(r.users.begin(), r.users.end(), [](const ChunkUser& p, const ChunkUser& q) {
          return std::pair(p.ino, p.index) < std::pair(q.ino, q.index);
        });
        out.emplace_back(k, encode_refs(r));
        return true;
      }
      out.emplace_back(k, Bytes(v.begin(), v.end()));
      return true;
    });
  };
  collect(a, x);
  collect(b, y);
  return x == y;
}

}  // namespace

bool Metadata::same_as(const Metadata& o) const {
  return segments == o.segments && counters == o.counters && next_inode == o.next_inode && last_txid == o.last_txid &&
         same_items(*tree_, *o.tree_);
}

bool Metadata::same_as(const LegacyMetadata& legacy) const {
  Options o = options_;
  o.account_nodes = false;
  Metadata other = from_legacy(legacy, o, std::make_shared<MemNodeIO>());
  bool segs = options_.account_nodes || segments == other.segments;
  MetaCounters c = counters;
  c.node_bytes = other.counters.node_bytes;  // (the legacy metadata has no nodes)
  return segs && c == other.counters && next_inode == other.next_inode && last_txid == other.last_txid &&
         same_items(*tree_, *other.tree_, true);
}

}  // namespace nanoer
