// The operations that change filesystem metadata, and what they refer to.
//
// Every change goes through Metadata::apply(Op). The filesystem layer applies
// operations as they happen and writes the same operations to the log;
// mounting replays them with the same apply(). So the state after a replay
// is, by construction, the state that was live. Operations are "physical"
// (set this entry, set this chunk), not "logical" (rename this path), which
// keeps replay simple and deterministic. Encoding: docs/format.md §8.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "nanoer/bytes.h"

namespace nanoer {

class Encoder;
class Decoder;

using InodeId = std::uint64_t;
inline constexpr InodeId kRootInode = 1;

inline constexpr std::size_t kMaxNameLength = 255;
inline constexpr std::size_t kMaxSymlinkLength = 4095;
inline constexpr std::size_t kMaxXattrNameLength = 255;
inline constexpr std::size_t kMaxXattrValueSize = 64 << 10;

// Where one chunk of a file is stored.
struct ChunkRef {
  std::uint64_t offset = 0;       // absolute offset of its data chunk record
  std::uint32_t stored_size = 0;  // bytes the record occupies (for space accounting)
  std::uint32_t size = 0;         // uncompressed bytes
  // With shared records (docs/format.md §8):
  std::uint32_t piece = 0;  // packs: where this chunk's bytes start in the pack
  bool packed = false;      // the record is a pack of several small files' data
  std::uint64_t hash = 0;   // dedup key of the data (hash_key()); 0: none
  bool cold = false;        // with cold data: recompressed in its cold form (tier.cpp)
  bool operator==(const ChunkRef&) const = default;
};

// The dedup key: the first 8 bytes of the data's BLAKE3, little-endian.
// Equal keys are only a hint; the full BLAKE3 is in the record.
inline std::uint64_t hash_key(const std::uint8_t* blake3) {
  std::uint64_t k = 0;
  for (int i = 7; i >= 0; --i) k = (k << 8) | blake3[i];
  return k;
}

struct InodeAttrs {
  InodeId id = 0;
  std::uint32_t mode = 0;  // st_mode: file type and permission bits
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::uint32_t nlink = 0;
  std::uint64_t size = 0;
  std::int64_t atime_ns = 0;
  std::int64_t mtime_ns = 0;
  std::int64_t ctime_ns = 0;
  std::int64_t btime_ns = 0;  // creation time
  std::uint64_t rdev = 0;
  InodeId parent = 0;     // directories only: the containing directory (root: itself)
  std::string symlink;    // symlinks only: the target
  bool is_dir() const;
  bool is_regular() const;
  bool is_symlink() const;
  bool operator==(const InodeAttrs&) const = default;
};

struct SegmentUsage {
  std::uint64_t seq = 0;
  std::uint64_t live_bytes = 0;  // bytes of data chunk records (and, with a tree, nodes) still referenced
  bool operator==(const SegmentUsage&) const = default;
};

// A chunk of a file: who refers to a record.
struct ChunkUser {
  InodeId ino = 0;
  std::uint64_t index = 0;
  bool operator==(const ChunkUser&) const = default;
};

namespace op {
struct PutInode { InodeAttrs attrs; };  // create, or replace the attributes
struct DeleteInode { InodeId ino; };    // and all its chunks and xattrs
struct SetEntry { InodeId dir; std::string name; InodeId ino; };  // add or replace
struct RemoveEntry { InodeId dir; std::string name; };
struct SetChunk { InodeId ino; std::uint64_t index; ChunkRef ref; };  // a new record
struct RemoveChunk { InodeId ino; std::uint64_t index; };
struct TruncateChunks { InodeId ino; std::uint64_t first_index; };  // remove index >= first
struct SetXattr { InodeId ino; std::string name; Bytes value; };
struct RemoveXattr { InodeId ino; std::string name; };
// Shared records only. Like SetChunk, with the extra fields of ChunkRef; with
// `share`, `ref` is a record that is already referenced (dedup), and gains a
// reference instead of being counted as new space.
//
// When `share` makes a record shared for the first time (it had a single
// reference), `owner` names that first reference (ChunkUser, above), so
// that the record's users are known without reading it (back-references, M6).
struct SetChunkRef {
  InodeId ino;
  std::uint64_t index;
  ChunkRef ref;
  bool share = false;
  std::optional<ChunkUser> owner;
};
}  // namespace op

using Op = std::variant<op::PutInode, op::DeleteInode, op::SetEntry, op::RemoveEntry,
                        op::SetChunk, op::RemoveChunk, op::TruncateChunks, op::SetXattr,
                        op::RemoveXattr, op::SetChunkRef>;

void encode_op(Encoder& out, const Op& op);
Op decode_op(Decoder& in);

}  // namespace nanoer
