#include "nanoer/metadata.h"

#include <algorithm>
#include <cstring>

#include <sys/stat.h>

#include "nanoer/encoding.h"
#include "nanoer/error.h"
#include "nanoer/io.h"

namespace nanoer {
namespace {

// Operation type codes; part of the on-disk format.
enum class OpCode : std::uint8_t {
  put_inode = 1,
  delete_inode = 2,
  set_entry = 3,
  remove_entry = 4,
  set_chunk = 5,
  remove_chunk = 6,
  truncate_chunks = 7,
  set_xattr = 8,
  remove_xattr = 9,
  set_chunk_ref = 10,  // shared records only
};

// Flags of set_chunk_ref and of version 2 checkpoint chunks.
namespace ref_flags {
constexpr std::uint64_t packed = 0x1;  // followed by the piece offset
constexpr std::uint64_t hash = 0x2;    // followed by the 8-byte dedup key
constexpr std::uint64_t share = 0x4;   // set_chunk_ref: a reference to an existing record;
                                       // checkpoint: the record has several references
constexpr std::uint64_t cold = 0x8;    // recompressed in its cold form (cold data only)
constexpr std::uint64_t known = packed | hash | share | cold;
}  // namespace ref_flags

constexpr char kCheckpointMagic[8] = {'N', 'N', 'R', 'C', 'K', 'P', 'T', '1'};
constexpr char kCheckpointMagicV2[8] = {'N', 'N', 'R', 'C', 'K', 'P', 'T', '2'};

[[noreturn]] void corrupt(const std::string& what) {
  throw Error(Errc::corrupt, "metadata: " + what);
}

std::uint32_t u32(std::uint64_t v) {
  if (v > UINT32_MAX) corrupt("value out of range");
  return static_cast<std::uint32_t>(v);
}

bool valid_name(std::string_view name) {
  return !name.empty() && name.size() <= kMaxNameLength && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

void encode_attrs(Encoder& out, const InodeAttrs& a) {
  out.varint(a.id);
  out.varint(a.mode);
  out.varint(a.uid);
  out.varint(a.gid);
  out.varint(a.nlink);
  out.varint(a.size);
  out.svarint(a.atime_ns);
  out.svarint(a.mtime_ns);
  out.svarint(a.ctime_ns);
  out.svarint(a.btime_ns);
  out.varint(a.rdev);
  out.varint(a.parent);
  out.str(a.symlink);
}

InodeAttrs decode_attrs(Decoder& in) {
  InodeAttrs a;
  a.id = in.varint();
  a.mode = u32(in.varint());
  a.uid = u32(in.varint());
  a.gid = u32(in.varint());
  a.nlink = u32(in.varint());
  a.size = in.varint();
  a.atime_ns = in.svarint();
  a.mtime_ns = in.svarint();
  a.ctime_ns = in.svarint();
  a.btime_ns = in.svarint();
  a.rdev = in.varint();
  a.parent = in.varint();
  a.symlink = in.str(kMaxSymlinkLength);
  if (a.id == 0) corrupt("inode number 0");
  return a;
}

void encode_ref(Encoder& out, const ChunkRef& r) {
  out.varint(r.offset);
  out.varint(r.stored_size);
  out.varint(r.size);
}

ChunkRef decode_ref(Decoder& in) {
  ChunkRef r;
  r.offset = in.varint();
  r.stored_size = u32(in.varint());
  r.size = u32(in.varint());
  return r;
}

// The extended form: flags, the three fields above, then what the flags say.
void encode_ref_ex(Encoder& out, const ChunkRef& r, bool share) {
  std::uint64_t flags = (r.packed ? ref_flags::packed : 0) | (r.hash ? ref_flags::hash : 0) |
                        (share ? ref_flags::share : 0) | (r.cold ? ref_flags::cold : 0);
  out.varint(flags);
  encode_ref(out, r);
  if (r.packed) out.varint(r.piece);
  if (r.hash) {
    std::uint8_t key[8];
    store_le(key, r.hash);
    out.raw(ByteView(key, 8));
  }
}

ChunkRef decode_ref_ex(Decoder& in, bool& share) {
  std::uint64_t flags = in.varint();
  if (flags & ~ref_flags::known) corrupt("unknown chunk flags");
  ChunkRef r = decode_ref(in);
  if (flags & ref_flags::packed) {
    r.packed = true;
    r.piece = u32(in.varint());
  }
  if (flags & ref_flags::hash) {
    r.hash = load_le<std::uint64_t>(in.raw(8).data());
    if (r.hash == 0) corrupt("zero dedup key");
  }
  share = flags & ref_flags::share;
  r.cold = flags & ref_flags::cold;
  return r;
}

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

bool Inode::is_dir() const { return S_ISDIR(mode); }
bool Inode::is_regular() const { return S_ISREG(mode); }
bool Inode::is_symlink() const { return S_ISLNK(mode); }

// ---- operations ------------------------------------------------------------

void encode_op(Encoder& out, const Op& op) {
  std::visit(Overloaded{
                 [&](const op::PutInode& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::put_inode));
                   encode_attrs(out, o.attrs);
                 },
                 [&](const op::DeleteInode& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::delete_inode));
                   out.varint(o.ino);
                 },
                 [&](const op::SetEntry& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::set_entry));
                   out.varint(o.dir);
                   out.str(o.name);
                   out.varint(o.ino);
                 },
                 [&](const op::RemoveEntry& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::remove_entry));
                   out.varint(o.dir);
                   out.str(o.name);
                 },
                 [&](const op::SetChunk& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::set_chunk));
                   out.varint(o.ino);
                   out.varint(o.index);
                   encode_ref(out, o.ref);
                 },
                 [&](const op::RemoveChunk& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::remove_chunk));
                   out.varint(o.ino);
                   out.varint(o.index);
                 },
                 [&](const op::TruncateChunks& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::truncate_chunks));
                   out.varint(o.ino);
                   out.varint(o.first_index);
                 },
                 [&](const op::SetXattr& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::set_xattr));
                   out.varint(o.ino);
                   out.str(o.name);
                   out.bytes(o.value);
                 },
                 [&](const op::RemoveXattr& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::remove_xattr));
                   out.varint(o.ino);
                   out.str(o.name);
                 },
                 [&](const op::SetChunkRef& o) {
                   out.u8(static_cast<std::uint8_t>(OpCode::set_chunk_ref));
                   out.varint(o.ino);
                   out.varint(o.index);
                   encode_ref_ex(out, o.ref, o.share);
                 },
             },
             op);
}

Op decode_op(Decoder& in) {
  switch (static_cast<OpCode>(in.u8())) {
    case OpCode::put_inode: return op::PutInode{decode_attrs(in)};
    case OpCode::delete_inode: return op::DeleteInode{in.varint()};
    case OpCode::set_entry: {
      InodeId dir = in.varint();
      std::string name = in.str(kMaxNameLength);
      return op::SetEntry{dir, std::move(name), in.varint()};
    }
    case OpCode::remove_entry: {
      InodeId dir = in.varint();
      return op::RemoveEntry{dir, in.str(kMaxNameLength)};
    }
    case OpCode::set_chunk: {
      InodeId ino = in.varint();
      std::uint64_t index = in.varint();
      return op::SetChunk{ino, index, decode_ref(in)};
    }
    case OpCode::remove_chunk: {
      InodeId ino = in.varint();
      return op::RemoveChunk{ino, in.varint()};
    }
    case OpCode::truncate_chunks: {
      InodeId ino = in.varint();
      return op::TruncateChunks{ino, in.varint()};
    }
    case OpCode::set_xattr: {
      InodeId ino = in.varint();
      std::string name = in.str(kMaxXattrNameLength);
      return op::SetXattr{ino, std::move(name), in.bytes(kMaxXattrValueSize)};
    }
    case OpCode::remove_xattr: {
      InodeId ino = in.varint();
      return op::RemoveXattr{ino, in.str(kMaxXattrNameLength)};
    }
    case OpCode::set_chunk_ref: {
      InodeId ino = in.varint();
      std::uint64_t index = in.varint();
      bool share = false;
      ChunkRef ref = decode_ref_ex(in, share);
      return op::SetChunkRef{ino, index, ref, share};
    }
  }
  corrupt("unknown operation");
}

// ---- Metadata --------------------------------------------------------------

Metadata Metadata::empty(std::uint64_t data_start, unsigned segment_shift, std::uint32_t uid,
                         std::uint32_t gid, std::int64_t now_ns, bool shared_records,
                         bool cold_data) {
  Metadata m(data_start, segment_shift, shared_records, cold_data);
  Inode root;
  root.id = kRootInode;
  root.mode = S_IFDIR | 0755;
  root.uid = uid;
  root.gid = gid;
  root.nlink = 2;
  root.atime_ns = root.mtime_ns = root.ctime_ns = root.btime_ns = now_ns;
  root.parent = kRootInode;
  m.inodes.emplace(kRootInode, std::move(root));
  return m;
}

Inode* Metadata::find(InodeId ino) {
  auto it = inodes.find(ino);
  return it == inodes.end() ? nullptr : &it->second;
}

const Inode* Metadata::find(InodeId ino) const {
  auto it = inodes.find(ino);
  return it == inodes.end() ? nullptr : &it->second;
}

void Metadata::claim(const ChunkRef& ref) {
  if (ref.offset < data_start_) corrupt("chunk location before the data area");
  auto it = segments.find(segment_of(ref.offset));
  if (it == segments.end()) corrupt("chunk in a segment that is not in use");
  it->second.live_bytes += ref.stored_size;
}

void Metadata::release(const ChunkRef& ref) {
  auto it = segments.find(segment_of(ref.offset));
  if (it == segments.end() || it->second.live_bytes < ref.stored_size) {
    corrupt("segment space accounting is inconsistent");
  }
  it->second.live_bytes -= ref.stored_size;
}

std::uint32_t Metadata::references(const ChunkRef& ref) const {
  auto it = shared.find(ref.offset);
  return it == shared.end() ? 1 : it->second;
}

namespace {
DedupTarget target_of(const ChunkRef& ref) { return {ref.offset, ref.packed, ref.piece}; }
}  // namespace

// A record's space is claimed by its first reference and released by its
// last. Only packs and records referenced more than once are counted.
void Metadata::add_ref(const ChunkRef& ref, bool share) {
  if (ref.hash) dedup.put(ref.hash, target_of(ref));
  if ((ref.packed || share || ref.hash) && !shared_records_) {
    corrupt("shared records on a filesystem without that feature");
  }
  if (ref.cold && !cold_data_) corrupt("cold data on a filesystem without that feature");
  if (ref.packed) {
    if (share) corrupt("a pack piece marked as shared");
    auto [it, first] = shared.try_emplace(ref.offset, 0);
    if (first) claim(ref);
    if (it->second == UINT32_MAX) corrupt("too many references to one record");
    ++it->second;
  } else if (share) {
    auto [it, first] = shared.try_emplace(ref.offset, 1);
    if (it->second == UINT32_MAX) corrupt("too many references to one record");
    ++it->second;
  } else {
    if (shared.contains(ref.offset)) corrupt("a new chunk at a location already in use");
    claim(ref);
  }
}

void Metadata::drop_ref(const ChunkRef& ref) {
  // Pieces are not counted one by one: a piece leaves the index with any
  // reference to it (a missed duplicate at worst); other data with its
  // record.
  if (ref.hash && ref.packed) dedup.erase(ref.hash, target_of(ref));
  auto it = shared.find(ref.offset);
  if (it == shared.end()) {
    if (ref.packed) corrupt("a pack piece of an unknown pack");
    if (ref.hash) dedup.erase(ref.hash, target_of(ref));
    release(ref);
    return;
  }
  --it->second;
  if (ref.packed) {
    if (it->second == 0) {
      shared.erase(it);
      release(ref);
    }
  } else if (it->second <= 1) {
    shared.erase(it);  // one reference left: no longer counted
  }
}

void Metadata::apply(const Op& op) {
  auto need = [&](InodeId ino) -> Inode& {
    Inode* inode = find(ino);
    if (!inode) corrupt("operation on missing inode " + std::to_string(ino));
    return *inode;
  };
  auto need_dir = [&](InodeId ino) -> Inode& {
    Inode& dir = need(ino);
    if (!dir.is_dir()) corrupt("directory operation on non-directory " + std::to_string(ino));
    return dir;
  };
  auto need_file = [&](InodeId ino) -> Inode& {
    Inode& file = need(ino);
    if (!file.is_regular()) corrupt("chunk operation on non-file " + std::to_string(ino));
    return file;
  };

  std::visit(
      Overloaded{
          [&](const op::PutInode& o) {
            if (o.attrs.id == 0) corrupt("inode number 0");
            Inode& inode = inodes[o.attrs.id];
            static_cast<InodeAttrs&>(inode) = o.attrs;
            next_inode = std::max(next_inode, o.attrs.id + 1);
          },
          [&](const op::DeleteInode& o) {
            if (o.ino == kRootInode) corrupt("deleting the root directory");
            Inode& inode = need(o.ino);
            for (const auto& [index, ref] : inode.chunks) drop_ref(ref);
            inodes.erase(o.ino);
          },
          [&](const op::SetEntry& o) {
            if (!valid_name(o.name)) corrupt("invalid file name");
            need(o.ino);
            need_dir(o.dir).entries.insert_or_assign(o.name, o.ino);
          },
          [&](const op::RemoveEntry& o) {
            Inode& dir = need_dir(o.dir);
            auto it = dir.entries.find(o.name);
            if (it == dir.entries.end()) corrupt("removing a missing directory entry");
            dir.entries.erase(it);
          },
          [&](const op::SetChunk& o) {
            if (o.ref.packed || o.ref.piece || o.ref.hash || o.ref.cold) corrupt("extended chunk fields in SetChunk");
            Inode& file = need_file(o.ino);
            auto [it, inserted] = file.chunks.try_emplace(o.index, o.ref);
            add_ref(o.ref, false);
            if (!inserted) {
              drop_ref(it->second);
              it->second = o.ref;
            }
          },
          [&](const op::SetChunkRef& o) {
            if (!o.ref.packed && o.ref.piece) corrupt("a piece offset outside a pack");
            Inode& file = need_file(o.ino);
            auto [it, inserted] = file.chunks.try_emplace(o.index, o.ref);
            add_ref(o.ref, o.share);
            if (!inserted) {
              drop_ref(it->second);
              it->second = o.ref;
            }
          },
          [&](const op::RemoveChunk& o) {
            Inode& file = need_file(o.ino);
            auto it = file.chunks.find(o.index);
            if (it == file.chunks.end()) return;
            drop_ref(it->second);
            file.chunks.erase(it);
          },
          [&](const op::TruncateChunks& o) {
            Inode& file = need_file(o.ino);
            auto it = file.chunks.lower_bound(o.first_index);
            for (auto i = it; i != file.chunks.end(); ++i) drop_ref(i->second);
            file.chunks.erase(it, file.chunks.end());
          },
          [&](const op::SetXattr& o) {
            need(o.ino).xattrs.insert_or_assign(o.name, o.value);
          },
          [&](const op::RemoveXattr& o) {
            Inode& inode = need(o.ino);
            if (inode.xattrs.erase(o.name) == 0) corrupt("removing a missing xattr");
          },
      },
      op);
}

// ---- checkpoints -----------------------------------------------------------

bool Metadata::is_v2_checkpoint(ByteView stream_start) {
  return stream_start.size() >= 8 && std::memcmp(stream_start.data(), kCheckpointMagicV2, 8) == 0;
}

Bytes Metadata::encode_checkpoint() const {
  Encoder out;
  out.raw(ByteView(
      reinterpret_cast<const std::uint8_t*>(shared_records_ ? kCheckpointMagicV2 : kCheckpointMagic), 8));
  out.varint(last_txid);
  out.varint(next_inode);
  out.varint(segments.size());
  for (const auto& [index, usage] : segments) {
    out.varint(index);
    out.varint(usage.seq);
    out.varint(usage.live_bytes);
  }
  // Sorted, so the same state always encodes to the same bytes.
  std::vector<InodeId> ids;
  ids.reserve(inodes.size());
  for (const auto& [id, inode] : inodes) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  out.varint(ids.size());
  for (InodeId id : ids) {
    const Inode& inode = inodes.at(id);
    encode_attrs(out, inode);
    out.varint(inode.chunks.size());
    for (const auto& [index, ref] : inode.chunks) {
      out.varint(index);
      if (shared_records_) {
        // "share" marks a record with several references, so that loading
        // counts only those.
        encode_ref_ex(out, ref, !ref.packed && shared.contains(ref.offset));
      } else {
        encode_ref(out, ref);
      }
    }
    out.varint(inode.entries.size());
    for (const auto& [name, ino] : inode.entries) {
      out.str(name);
      out.varint(ino);
    }
    out.varint(inode.xattrs.size());
    for (const auto& [name, value] : inode.xattrs) {
      out.str(name);
      out.bytes(value);
    }
  }
  return out.take();
}

Metadata Metadata::decode_checkpoint(ByteView data, std::uint64_t data_start,
                                     unsigned segment_shift, bool shared_records, bool cold_data) {
  Decoder in(data);
  ByteView magic = in.raw(8);
  bool v2 = std::memcmp(magic.data(), kCheckpointMagicV2, 8) == 0;
  if (!v2 && std::memcmp(magic.data(), kCheckpointMagic, 8) != 0) corrupt("not a checkpoint");
  if (v2 && !shared_records) corrupt("checkpoint with shared records on a filesystem without them");
  Metadata m(data_start, segment_shift, shared_records, cold_data);
  m.last_txid = in.varint();
  m.next_inode = in.varint();

  std::uint64_t segment_count = in.varint();
  std::map<std::uint64_t, std::uint64_t> recorded_live;
  for (std::uint64_t i = 0; i < segment_count; ++i) {
    std::uint64_t index = in.varint();
    std::uint64_t seq = in.varint();
    recorded_live[index] = in.varint();
    if (!m.segments.emplace(index, SegmentUsage{seq, 0}).second) corrupt("duplicate segment");
  }

  // Records with several references, and packs: their references are
  // counted here, and their space claimed once at the end.
  struct Counted {
    ChunkRef ref;
    std::uint32_t count = 0;
  };
  std::unordered_map<std::uint64_t, Counted> counted;
  std::uint64_t inode_count = in.varint();
  for (std::uint64_t i = 0; i < inode_count; ++i) {
    Inode inode;
    static_cast<InodeAttrs&>(inode) = decode_attrs(in);
    std::uint64_t chunks = in.varint();
    for (std::uint64_t c = 0; c < chunks; ++c) {
      std::uint64_t index = in.varint();
      bool share = false;
      ChunkRef ref = v2 ? decode_ref_ex(in, share) : decode_ref(in);
      if (!ref.packed && ref.piece) corrupt("a piece offset outside a pack");
      if (ref.cold && !m.cold_data_) corrupt("cold data on a filesystem without that feature");
      if (ref.hash) m.dedup.put(ref.hash, target_of(ref));
      if (ref.packed || share) {
        auto it = counted.try_emplace(ref.offset, Counted{ref, 0}).first;
        if (it->second.ref.stored_size != ref.stored_size || it->second.ref.packed != ref.packed) {
          corrupt("references to one record disagree");
        }
        if (it->second.count == UINT32_MAX) corrupt("too many references to one record");
        ++it->second.count;
      } else {
        m.claim(ref);
      }
      if (!inode.chunks.emplace(index, ref).second) corrupt("duplicate chunk");
    }
    std::uint64_t entries = in.varint();
    for (std::uint64_t e = 0; e < entries; ++e) {
      std::string name = in.str(kMaxNameLength);
      if (!valid_name(name)) corrupt("invalid file name");
      inode.entries.emplace(std::move(name), in.varint());
    }
    std::uint64_t xattrs = in.varint();
    for (std::uint64_t x = 0; x < xattrs; ++x) {
      std::string name = in.str(kMaxXattrNameLength);
      inode.xattrs.emplace(std::move(name), in.bytes(kMaxXattrValueSize));
    }
    InodeId id = inode.id;
    if (id >= m.next_inode) corrupt("inode number beyond the allocation counter");
    if (!m.inodes.emplace(id, std::move(inode)).second) corrupt("duplicate inode");
  }
  if (!in.done()) corrupt("trailing bytes in checkpoint");
  for (const auto& [offset, c] : counted) {
    if (!c.ref.packed && c.count < 2) corrupt("a record marked shared has one reference");
    m.claim(c.ref);
    m.shared.emplace(offset, c.count);
  }

  // Cross-checks: every reference resolves, and the space accounting that
  // was recorded matches what the chunks add up to.
  const Inode* root = m.find(kRootInode);
  if (!root || !root->is_dir()) corrupt("checkpoint has no root directory");
  for (const auto& [id, inode] : m.inodes) {
    for (const auto& [name, child] : inode.entries) {
      if (!inode.is_dir() || !m.find(child)) corrupt("dangling directory entry");
    }
    if (!inode.chunks.empty() && !inode.is_regular()) corrupt("chunks on a non-file");
  }
  for (const auto& [index, usage] : m.segments) {
    if (recorded_live[index] != usage.live_bytes) corrupt("segment space accounting mismatch");
  }
  return m;
}

}  // namespace nanoer
