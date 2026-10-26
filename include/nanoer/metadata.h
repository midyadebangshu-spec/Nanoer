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
  // With shared records (docs/format.md §8):
  std::uint32_t piece = 0;  // packs: where this chunk's bytes start in the pack
  bool packed = false;      // the record is a pack of several small files' data
  std::uint64_t hash = 0;   // dedup key of the data (hash_key()); 0: none
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
struct SetChunk { InodeId ino; std::uint64_t index; ChunkRef ref; };  // a new record
struct RemoveChunk { InodeId ino; std::uint64_t index; };
struct TruncateChunks { InodeId ino; std::uint64_t first_index; };  // remove index >= first
struct SetXattr { InodeId ino; std::string name; Bytes value; };
struct RemoveXattr { InodeId ino; std::string name; };
// Shared records only. Like SetChunk, with the extra fields of ChunkRef; with
// `share`, `ref` is a record that is already referenced (dedup), and gains a
// reference instead of being counted as new space.
struct SetChunkRef { InodeId ino; std::uint64_t index; ChunkRef ref; bool share = false; };
}  // namespace op

using Op = std::variant<op::PutInode, op::DeleteInode, op::SetEntry, op::RemoveEntry,
                        op::SetChunk, op::RemoveChunk, op::TruncateChunks, op::SetXattr,
                        op::RemoveXattr, op::SetChunkRef>;

void encode_op(Encoder& out, const Op& op);
Op decode_op(Decoder& in);

class Metadata {
 public:
  // `shared_records`: the filesystem has that feature (records may be shared
  // or be packs); otherwise SetChunkRef is invalid and checkpoints use the
  // version 1 encoding.
  Metadata(std::uint64_t data_start, unsigned segment_shift, bool shared_records = false)
      : data_start_(data_start), segment_shift_(segment_shift), shared_records_(shared_records) {}

  // A new filesystem: just the root directory.
  static Metadata empty(std::uint64_t data_start, unsigned segment_shift, std::uint32_t uid,
                        std::uint32_t gid, std::int64_t now_ns, bool shared_records = false);

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
                                    unsigned segment_shift, bool shared_records = false);

  // Whether a snapshot stream (or its first bytes) uses the version 2
  // encoding, which only filesystems with shared records write.
  static bool is_v2_checkpoint(ByteView stream_start);

  bool shared_records() const { return shared_records_; }
  // References to the record at `ref.offset` (1 unless listed in `shared`).
  std::uint32_t references(const ChunkRef& ref) const;

  bool operator==(const Metadata&) const = default;

  std::unordered_map<InodeId, Inode> inodes;
  // Every segment not free: holds log records, live data, or both.
  std::map<std::uint64_t, SegmentUsage> segments;
  // Every pack, and every other record with more than one reference: record
  // offset -> number of references. Any other referenced record has one.
  // A record's space is counted once, however many references it has.
  std::unordered_map<std::uint64_t, std::uint32_t> shared;
  InodeId next_inode = kRootInode + 1;
  std::uint64_t last_txid = 0;  // the last transaction applied

 private:
  void release(const ChunkRef& ref);
  void claim(const ChunkRef& ref);
  void add_ref(const ChunkRef& ref, bool share);
  void drop_ref(const ChunkRef& ref);

  std::uint64_t data_start_;
  unsigned segment_shift_;
  bool shared_records_;
};

}  // namespace nanoer
