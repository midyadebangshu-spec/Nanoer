// The log: one append-only stream of records spread over segments.
//
// Data chunks, metadata transactions and checkpoints are all appended to the
// same log, in order. When a segment fills up, a seal record at its end names
// the segment where the log continues. Replay reads the log from the latest
// checkpoint and stops at the first record that is not intact, and enters a
// new segment only through an intact seal, so the recovered state is always
// a prefix of what was written: a state that really existed (KNOWLEDGE.md §7,
// docs/format.md §6–§10).
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "nanoer/bytes.h"
#include "nanoer/codec.h"
#include "nanoer/format.h"
#include "nanoer/metadata.h"
#include "nanoer/record.h"
#include "nanoer/segment.h"

namespace nanoer {

class Store;

inline constexpr std::size_t kSealExtraSize = 16;
inline constexpr std::size_t kSealRecordSize = kRecordHeaderSize + kSealExtraSize;
inline constexpr std::size_t kTransactionExtraSize = 16;
inline constexpr std::size_t kCheckpointExtraSize = 32;

namespace checkpoint_flags {
inline constexpr std::uint32_t final_part = 0x01;
}

// A position in the log.
struct LogPosition {
  std::uint64_t index = 0;   // segment
  std::uint64_t seq = 0;     // that segment's sequence number
  std::uint64_t offset = 0;  // absolute byte offset
  bool operator==(const LogPosition&) const = default;
};

class Journal {
 public:
  // `meta.segments` is the set of segments in use; the journal adds the
  // segments it starts and allocates only segments outside that set.
  Journal(Store& store, const Superblock& sb, Metadata& meta, std::uint64_t next_seq);

  // Starts a brand-new log (mkfs).
  void start();
  // Continues after replay: seals the replayed log at `end` and moves to a
  // fresh segment, so nothing is ever appended after a possibly torn tail.
  void resume(const LogPosition& end);

  ChunkRef append_chunk(const EncodedChunk& chunk, InodeId ino, std::uint64_t file_offset);
  LogPosition append_transaction(std::uint64_t txid, const std::vector<Op>& ops);
  // Writes a complete snapshot of `meta`, split into parts as needed.
  // Returns where its first part starts (what the superblock points to).
  LogPosition append_checkpoint(const Metadata& meta);

  void write_buffered();  // hand appended records to the store
  void flush();           // and make them durable

  // Call once a checkpoint starting at `start` is durable and the superblock
  // points to it. Segments released at the previous checkpoint become free;
  // segments now entirely dead and older than `start` are released, to
  // become free at the next checkpoint (docs/format.md §10).
  void checkpoint_done(const LogPosition& start);

  // At mount, from the checkpoint at `start`: segments with no live data
  // that are older than it are free at once. The superblock pointing at
  // `start` is durable and stays valid until two more checkpoints are
  // written, so no older checkpoint can be fallen back to any more.
  void release_after_mount(const LogPosition& start, std::uint64_t log_end_index);

  // Normal appends leave this many free segments untouched, so a checkpoint
  // (which is what frees space) can always be written.
  void set_reserve(std::uint64_t segments) { reserve_ = segments; }
  std::uint64_t reserve() const { return reserve_; }

  std::uint64_t free_segments() const;
  std::uint64_t pending_free_segments() const { return pending_free_.size(); }
  std::uint64_t current_remaining() const;
  std::uint64_t current_index() const { return writer_->index(); }
  std::uint64_t next_seq() const { return next_seq_; }
  std::uint64_t bytes_since_checkpoint() const { return bytes_since_checkpoint_; }
  // The largest transaction payload that is guaranteed to fit.
  std::size_t max_transaction_payload() const;

 private:
  LogPosition append_record(RecordType type, std::uint8_t flags, ByteView extra, ByteView payload,
                            bool may_use_reserve);
  void ensure_room(std::uint64_t record_size, bool may_use_reserve);
  void roll_over(bool may_use_reserve);
  std::uint64_t allocate(bool may_use_reserve);
  void open_segment(std::uint64_t index);

  Store& store_;
  const Superblock& sb_;
  Metadata& meta_;
  std::uint64_t next_seq_;
  std::uint64_t reserve_ = 0;
  std::uint64_t bytes_since_checkpoint_ = 0;
  std::set<std::uint64_t> pending_free_;
  std::optional<SegmentWriter> writer_;
};

struct ReplayResult {
  Metadata meta;
  LogPosition checkpoint;          // where the checkpoint replay started from begins
  LogPosition end;                 // where the intact log ends
  std::uint64_t max_seq = 0;       // highest sequence number of any segment header found
  std::uint64_t transactions = 0;  // replayed after the checkpoint
  std::uint64_t log_bytes = 0;     // of records after the checkpoint
  std::uint64_t checkpoint_bytes = 0;
};

// Reads the latest checkpoint and replays the log after it. Throws
// Error(corrupt) if the checkpoint is missing or damaged, or the log
// contradicts itself.
ReplayResult replay(Store& store, const Superblock& sb);

}  // namespace nanoer
