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
#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/legacy_metadata.h"
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

// The newest checkpoint cannot be read. Mounting may fall back to the
// previous one (the other superblock slot).
class CheckpointError : public Error {
 public:
  explicit CheckpointError(const std::string& message) : Error(Errc::corrupt, message) {}
};

// A transaction in the log contradicts the metadata (it cannot be applied).
// Replaying again with stop_before_txid = txid() gives the state before it.
class ReplayError : public Error {
 public:
  ReplayError(std::uint64_t txid, const std::string& message)
      : Error(Errc::corrupt, message), txid_(txid) {}
  std::uint64_t txid() const { return txid_; }

 private:
  std::uint64_t txid_;
};

class Journal;

// Tree nodes in the log: read from the store, written through the journal.
class LogNodeIO final : public NodeIO {
 public:
  LogNodeIO(Store& store, const Superblock& sb) : store_(store), sb_(sb) {}
  void set_journal(Journal* journal) { journal_ = journal; }
  Bytes read(const NodeRef& ref, std::uint64_t seq) override;
  NodeRef write(ByteView encoded, const NodeInfo& info) override;

 private:
  Store& store_;
  const Superblock sb_;  // a copy: only the geometry is used, which never changes
  Journal* journal_ = nullptr;
};

// A position in the log.
struct LogPosition {
  std::uint64_t index = 0;   // segment
  std::uint64_t seq = 0;     // that segment's sequence number
  std::uint64_t offset = 0;  // absolute byte offset
  bool operator==(const LogPosition&) const = default;
};

class Journal {
 public:
  // `segments` is the set of segments in use; the journal adds the
  // segments it starts and allocates only segments outside that set.
  Journal(Store& store, const Superblock& sb, std::map<std::uint64_t, SegmentUsage>& segments,
          std::uint64_t next_seq);

  // Starts a brand-new log (mkfs).
  void start();
  // Continues after replay: seals the replayed log at `end` and moves to a
  // fresh segment, so nothing is ever appended after a possibly torn tail.
  void resume(const LogPosition& end, std::uint64_t last_txid);

  // With `blake3`, the record carries the data's identity and the returned
  // reference its dedup key.
  // With `pack`, the record holds several small files' data (no owner).
  ChunkRef append_chunk(const EncodedChunk& chunk, InodeId ino, std::uint64_t file_offset,
                        const std::optional<Blake3Hash>& blake3 = std::nullopt, bool pack = false);
  // GC: appends a copy of an intact data chunk record (same header fields
  // and payload, the current segment's sequence number). Returns where.
  // `urgent`: may use one of the reserve segments. Throws Error(no_space)
  // before writing anything if there is no room.
  // `owner`: the user the copy's header names instead (not for packs).
  std::uint64_t append_copy(const Record& record, bool urgent, const std::optional<ChunkUser>& owner = std::nullopt);
  // GC: a pack built again from the live pieces of an old one.
  ChunkRef append_repacked(const EncodedChunk& pack, bool urgent);
  LogPosition append_transaction(std::uint64_t txid, const std::vector<Op>& ops);
  // A metadata tree node (compressed). Like checkpoints, it may use the
  // reserve: nodes are written by checkpoints, which free space.
  NodeRef append_node(ByteView encoded, const NodeInfo& info);
  // Writes the tree's changed nodes, then the checkpoint record (split into
  // parts as needed). Returns where its first part starts (what the
  // superblock points to).
  LogPosition append_checkpoint(Metadata& meta);
  // A filesystem from before M6: a full snapshot (mkfs of old formats, tests).
  LogPosition append_legacy_checkpoint(const LegacyMetadata& meta);

  void write_buffered();  // hand appended records to the store
  void flush();           // and make them durable

  // The newest transaction known to be durable (written before the last
  // flush). Every transaction record carries it, so that replay can tell
  // damage from a torn write (docs/format.md §10.2).
  std::uint64_t durable_txid() const { return durable_txid_; }

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

  // Discard (docs/M6_PLAN.md §5): a segment that becomes free is queued, and
  // discard_some() tells the store it is no longer needed. Only free
  // segments are queued (a free segment is one the log may write over at
  // any time, so zeroing it loses nothing), and one the log takes back is
  // dropped from the queue. Off until enabled.
  void enable_discard() { discard_on_ = true; }
  // At mount: the free segments among `written` (those whose header is
  // still intact, so they may hold data: freed before a crash, or before
  // discard was enabled).
  void queue_discard(const std::vector<std::uint64_t>& written);
  // Discards up to `max` queued segments; returns how many.
  std::size_t discard_some(std::size_t max);
  std::size_t discard_queued() const { return discard_.size(); }
  std::uint64_t discarded_segments() const { return discarded_; }

  // Normal appends leave this many free segments untouched, so a checkpoint
  // (which is what frees space) can always be written.
  // `gc_segments` of them may be used by garbage collection when space is
  // urgent (append_copy).
  void set_reserve(std::uint64_t segments, std::uint64_t gc_segments = 0) {
    reserve_ = segments;
    gc_reserve_ = segments > gc_segments ? segments - gc_segments : 0;
  }
  std::uint64_t reserve() const { return reserve_; }

  std::uint64_t free_segments() const;
  // Bytes of records append_copy() can take before it runs out of room.
  std::uint64_t copy_room(bool urgent) const;
  std::uint64_t pending_free_segments() const { return pending_free_.size(); }
  std::uint64_t current_remaining() const;
  std::uint64_t current_index() const { return writer_->index(); }
  std::uint64_t next_seq() const { return next_seq_; }
  std::uint64_t bytes_since_checkpoint() const { return bytes_since_checkpoint_; }
  // The largest transaction payload that is guaranteed to fit.
  std::size_t max_transaction_payload() const;

 private:
  LogPosition append_checkpoint_stream(ByteView stream, std::uint64_t txid);
  // `keep_free`: refuse to start a segment unless more than this many
  // are free (the reserve for normal writes, 0 for what frees space).
  LogPosition append_record(RecordType type, std::uint8_t flags, ByteView extra, ByteView payload,
                            std::uint64_t keep_free);
  void ensure_room(std::uint64_t record_size, std::uint64_t keep_free);
  void roll_over(std::uint64_t keep_free);
  std::uint64_t allocate(std::uint64_t keep_free);
  void open_segment(std::uint64_t index);

  Store& store_;
  const Superblock& sb_;
  std::map<std::uint64_t, SegmentUsage>& segments_;
  std::uint64_t next_seq_;
  std::uint64_t reserve_ = 0;
  std::uint64_t gc_reserve_ = 0;  // what GC leaves free when space is urgent
  std::uint64_t bytes_since_checkpoint_ = 0;
  std::uint64_t appended_txid_ = 0;  // newest transaction appended
  std::uint64_t durable_txid_ = 0;   // newest transaction made durable by a flush
  std::set<std::uint64_t> pending_free_;
  bool discard_on_ = false;
  std::set<std::uint64_t> discard_;
  std::uint64_t discarded_ = 0;
  std::optional<SegmentWriter> writer_;
};

// Evidence that the log was damaged, not merely cut short by a crash.
struct LogDamage {
  bool found = false;
  std::uint64_t offset = 0;        // where the intact log stops
  std::uint64_t durable_claim = 0;  // newest transaction later records say was durable
};

struct ReplayResult {
  Metadata meta;
  // The filesystem predates the metadata tree: `meta` was converted in
  // memory (so it can only be mounted read-only), and this is what was read.
  std::optional<LegacyMetadata> legacy;
  LogPosition checkpoint;          // where the checkpoint replay started from begins
  LogPosition end;                 // where the intact log ends
  std::uint64_t max_seq = 0;       // highest sequence number of any segment header found
  std::uint64_t transactions = 0;  // replayed after the checkpoint
  std::uint64_t log_bytes = 0;     // of records after the checkpoint
  std::uint64_t checkpoint_bytes = 0;
  // Set when intact records after the end show that durable transactions
  // were lost: the log was damaged, and the state is only a prefix.
  LogDamage damage;
  // Segments whose header is intact (this filesystem's): any of them that
  // is free may still hold old records (discard).
  std::vector<std::uint64_t> written;
};

// Reads the latest checkpoint and replays the log after it. Throws
// CheckpointError if the checkpoint is missing or damaged, and Error(corrupt)
// if the log contradicts itself. Damage that cuts the log short is reported
// in ReplayResult::damage rather than thrown.
// `stop_before_txid` ends the replay just before that transaction (fsck).
ReplayResult replay(Store& store, const Superblock& sb,
                    std::uint64_t stop_before_txid = UINT64_MAX, const BTree::Options& tree = {});
// The Metadata options a superblock's features call for.
Metadata::Options metadata_options(const Superblock& sb, const BTree::Options& tree = {});

}  // namespace nanoer
