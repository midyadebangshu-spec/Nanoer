// In-memory filesystem metadata (M2), and the operations that change it.
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
  bool operator==(const ChunkRef&) const = default;
};

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
  bool operator==(const InodeAttrs&) const = default;
};

struct Inode : InodeAttrs {
  // Regular files: chunk index -> location. Chunk i covers bytes
  // [i * chunk_size, (i + 1) * chunk_size). A missing chunk is a hole.
  std::map<std::uint64_t, ChunkRef> chunks;
  // Directories: name -> inode ("." and ".." are implied, not stored).
  std::map<std::string, InodeId, std::less<>> entries;
  std::map<std::string, Bytes, std::less<>> xattrs;

  bool is_dir() const;
  bool is_regular() const;
  bool is_symlink() const;
  bool operator==(const Inode&) const = default;
};

struct SegmentUsage {
  std::uint64_t seq = 0;
  std::uint64_t live_bytes = 0;  // bytes of data chunk records still referenced
  bool operator==(const SegmentUsage&) const = default;
};

namespace op {
struct PutInode { InodeAttrs attrs; };  // create, or replace the attributes
struct DeleteInode { InodeId ino; };    // and all its chunks and xattrs
struct SetEntry { InodeId dir; std::string name; InodeId ino; };  // add or replace
struct RemoveEntry { InodeId dir; std::string name; };
struct SetChunk { InodeId ino; std::uint64_t index; ChunkRef ref; };
struct RemoveChunk { InodeId ino; std::uint64_t index; };
struct TruncateChunks { InodeId ino; std::uint64_t first_index; };  // remove index >= first
struct SetXattr { InodeId ino; std::string name; Bytes value; };
struct RemoveXattr { InodeId ino; std::string name; };
}  // namespace op

using Op = std::variant<op::PutInode, op::DeleteInode, op::SetEntry, op::RemoveEntry,
                        op::SetChunk, op::RemoveChunk, op::TruncateChunks, op::SetXattr,
                        op::RemoveXattr>;

void encode_op(Encoder& out, const Op& op);
Op decode_op(Decoder& in);

class Metadata {
 public:
  Metadata(std::uint64_t data_start, unsigned segment_shift)
      : data_start_(data_start), segment_shift_(segment_shift) {}

  // A new filesystem: just the root directory.
  static Metadata empty(std::uint64_t data_start, unsigned segment_shift, std::uint32_t uid,
                        std::uint32_t gid, std::int64_t now_ns);

  // Applies one change. Throws Error(corrupt) if it does not fit the current
  // state (during replay that means the log is damaged).
  void apply(const Op& op);

  Inode* find(InodeId ino);
  const Inode* find(InodeId ino) const;

  std::uint64_t segment_of(std::uint64_t offset) const {
    return (offset - data_start_) >> segment_shift_;
  }

  // A complete snapshot, for checkpoints (docs/format.md §9).
  Bytes encode_checkpoint() const;
  static Metadata decode_checkpoint(ByteView data, std::uint64_t data_start,
                                    unsigned segment_shift);

  bool operator==(const Metadata&) const = default;

  std::unordered_map<InodeId, Inode> inodes;
  // Every segment not free: holds log records, live data, or both.
  std::map<std::uint64_t, SegmentUsage> segments;
  InodeId next_inode = kRootInode + 1;
  std::uint64_t last_txid = 0;  // the last transaction applied

 private:
  void release(const ChunkRef& ref);
  void claim(const ChunkRef& ref);

  std::uint64_t data_start_;
  unsigned segment_shift_;
};

}  // namespace nanoer
