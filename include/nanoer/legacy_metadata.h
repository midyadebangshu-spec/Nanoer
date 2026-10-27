// Filesystem metadata held entirely in memory, as M2-M5 kept it: loaded
// from a full checkpoint and the log after it. Kept to read filesystems
// made before M6 (nanoer upgrade, read-only mounts), for fsck, and as the
// oracle the tree-backed Metadata is tested against.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include "nanoer/bytes.h"
#include "nanoer/dedup.h"
#include "nanoer/ops.h"

namespace nanoer {

struct Inode : InodeAttrs {
  // Regular files: chunk index -> location. Chunk i covers bytes
  // [i * chunk_size, (i + 1) * chunk_size). A missing chunk is a hole.
  std::map<std::uint64_t, ChunkRef> chunks;
  // Directories: name -> inode ("." and ".." are implied, not stored).
  std::map<std::string, InodeId, std::less<>> entries;
  std::map<std::string, Bytes, std::less<>> xattrs;

  bool operator==(const Inode&) const = default;
};


class LegacyMetadata {
 public:
  // `shared_records`: the filesystem has that feature (records may be shared
  // or be packs); otherwise SetChunkRef is invalid and checkpoints use the
  // version 1 encoding.
  // `cold_data`: references may be marked cold.
  LegacyMetadata(std::uint64_t data_start, unsigned segment_shift, bool shared_records = false,
           bool cold_data = false)
      : data_start_(data_start),
        segment_shift_(segment_shift),
        shared_records_(shared_records),
        cold_data_(shared_records && cold_data) {}

  // A new filesystem: just the root directory.
  static LegacyMetadata empty(std::uint64_t data_start, unsigned segment_shift, std::uint32_t uid,
                        std::uint32_t gid, std::int64_t now_ns, bool shared_records = false,
                        bool cold_data = false);

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
  static LegacyMetadata decode_checkpoint(ByteView data, std::uint64_t data_start,
                                    unsigned segment_shift, bool shared_records = false,
                                    bool cold_data = false);

  // Whether a snapshot stream (or its first bytes) uses the version 2
  // encoding, which only filesystems with shared records write.
  static bool is_v2_checkpoint(ByteView stream_start);

  bool shared_records() const { return shared_records_; }
  std::uint64_t data_start() const { return data_start_; }
  unsigned segment_shift() const { return segment_shift_; }
  bool cold_data() const { return cold_data_; }
  // References to the record at `ref.offset` (1 unless listed in `shared`).
  std::uint32_t references(const ChunkRef& ref) const;

  bool operator==(const LegacyMetadata&) const = default;

  std::unordered_map<InodeId, Inode> inodes;
  // Every segment not free: holds log records, live data, or both.
  std::map<std::uint64_t, SegmentUsage> segments;
  // Every pack, and every other record with more than one reference: record
  // offset -> number of references. Any other referenced record has one.
  // A record's space is counted once, however many references it has.
  std::unordered_map<std::uint64_t, std::uint32_t> shared;
  // Live data by content (dedup key -> record), kept by apply() and rebuilt
  // when a checkpoint is loaded.
  DedupIndex dedup;
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
  bool cold_data_;
};

}  // namespace nanoer
