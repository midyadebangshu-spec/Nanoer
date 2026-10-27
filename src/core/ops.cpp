#include "nanoer/ops.h"

#include <sys/stat.h>

#include "meta_encoding.h"
#include "nanoer/encoding.h"
#include "nanoer/error.h"
#include "nanoer/io.h"

namespace nanoer {

bool InodeAttrs::is_dir() const { return S_ISDIR(mode); }
bool InodeAttrs::is_regular() const { return S_ISREG(mode); }
bool InodeAttrs::is_symlink() const { return S_ISLNK(mode); }

namespace meta {
namespace {

// Flags of set_chunk_ref and of version 2 checkpoint chunks.
namespace ref_flags {
constexpr std::uint64_t packed = 0x1;  // followed by the piece offset
constexpr std::uint64_t hash = 0x2;    // followed by the 8-byte dedup key
constexpr std::uint64_t share = 0x4;   // set_chunk_ref: a reference to an existing record;
                                       // checkpoint: the record has several references
constexpr std::uint64_t cold = 0x8;    // recompressed in its cold form (cold data only)
constexpr std::uint64_t owner = 0x10;  // set_chunk_ref: followed by the first reference (M6)
constexpr std::uint64_t known = packed | hash | share | cold | owner;
}  // namespace ref_flags


}  // namespace

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
void encode_ref_ex(Encoder& out, const ChunkRef& r, bool share, const std::optional<ChunkUser>& owner) {
  std::uint64_t flags = (r.packed ? ref_flags::packed : 0) | (r.hash ? ref_flags::hash : 0) |
                        (share ? ref_flags::share : 0) | (r.cold ? ref_flags::cold : 0) |
                        (owner ? ref_flags::owner : 0);
  out.varint(flags);
  encode_ref(out, r);
  if (r.packed) out.varint(r.piece);
  if (r.hash) {
    std::uint8_t key[8];
    store_le(key, r.hash);
    out.raw(ByteView(key, 8));
  }
  if (owner) {
    out.varint(owner->ino);
    out.varint(owner->index);
  }
}

ChunkRef decode_ref_ex(Decoder& in, bool& share, std::optional<ChunkUser>* owner) {
  std::uint64_t flags = in.varint();
  if (flags & ~ref_flags::known) corrupt("unknown chunk flags");
  if ((flags & ref_flags::owner) && !owner) corrupt("unknown chunk flags");
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
  if (flags & ref_flags::owner) {
    ChunkUser u;
    u.ino = in.varint();
    u.index = in.varint();
    *owner = u;
  }
  return r;
}

}  // namespace meta

namespace {

using namespace meta;

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

}  // namespace

// ---- operations ------------------------------------------------------------

void encode_op(Encoder& out, const Op& op) {
  std::visit(meta::Overloaded{
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
                   encode_ref_ex(out, o.ref, o.share, o.owner);
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
      std::optional<ChunkUser> owner;
      ChunkRef ref = decode_ref_ex(in, share, &owner);
      return op::SetChunkRef{ino, index, ref, share, owner};
    }
  }
  corrupt("unknown operation");
}


}  // namespace nanoer
