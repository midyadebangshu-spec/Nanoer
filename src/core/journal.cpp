#include "nanoer/journal.h"

#include "nanoer/checksum.h"
#include "nanoer/encoding.h"
#include "nanoer/error.h"
#include "nanoer/io.h"

namespace nanoer {
namespace {

// Checkpoint parts are at most this big; smaller parts waste less of a
// segment's tail when a snapshot does not fit in what is left of it.
constexpr std::uint64_t kMaxCheckpointPart = 4u << 20;
constexpr std::uint64_t kMinCheckpointPart = 64u << 10;

std::uint64_t record_size(std::size_t extra, std::size_t payload) {
  return align_up(kRecordHeaderSize + extra + payload, kRecordAlignment);
}

[[noreturn]] void corrupt(const std::string& what) { throw Error(Errc::corrupt, "log: " + what); }

}  // namespace

// ---- Journal ---------------------------------------------------------------

Journal::Journal(Store& store, const Superblock& sb, Metadata& meta, std::uint64_t next_seq)
    : store_(store), sb_(sb), meta_(meta), next_seq_(next_seq) {}

void Journal::start() { open_segment(allocate(true)); }

void Journal::resume(const LogPosition& end) {
  writer_.emplace(SegmentWriter::resume(store_, sb_, end.index, end.seq, end.offset));
  roll_over(true);
  flush();
}

std::uint64_t Journal::free_segments() const {
  std::uint64_t used = meta_.segments.size() + pending_free_.size();
  return used >= sb_.segment_count ? 0 : sb_.segment_count - used;
}

std::uint64_t Journal::current_remaining() const {
  std::uint64_t r = writer_->remaining();
  return r > kSealRecordSize ? r - kSealRecordSize : 0;
}

std::size_t Journal::max_transaction_payload() const {
  return static_cast<std::size_t>(sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize -
                                  kRecordHeaderSize - kTransactionExtraSize - kRecordAlignment);
}

std::uint64_t Journal::allocate(bool may_use_reserve) {
  if (!may_use_reserve && free_segments() <= reserve_) {
    throw Error(Errc::no_space, "the filesystem is full");
  }
  for (std::uint64_t i = 0; i < sb_.segment_count; ++i) {
    if (!meta_.segments.contains(i) && !pending_free_.contains(i)) return i;
  }
  throw Error(Errc::no_space, "the filesystem is full");
}

void Journal::open_segment(std::uint64_t index) {
  std::uint64_t seq = next_seq_++;
  if (writer_) writer_->write_buffered();
  writer_.emplace(store_, sb_, index, seq);
  meta_.segments[index] = SegmentUsage{seq, 0};
}

void Journal::roll_over(bool may_use_reserve) {
  std::uint64_t next = allocate(may_use_reserve);
  std::uint8_t extra[kSealExtraSize];
  store_le(extra, next);
  store_le(extra + 8, next_seq_);  // the sequence number open_segment() will give it
  Bytes seal = encode_record(RecordType::segment_seal, 0, writer_->seq(), ByteView(extra), {});
  if (!writer_->append(seal)) throw Error(Errc::io, "no room left for a segment seal");
  open_segment(next);
}

void Journal::ensure_room(std::uint64_t size, bool may_use_reserve) {
  // Every segment keeps room for its seal at the end.
  if (size + kSealRecordSize <= writer_->remaining()) return;
  roll_over(may_use_reserve);
  if (size + kSealRecordSize > writer_->remaining()) {
    throw Error(Errc::invalid_argument, "record larger than a segment");
  }
}

LogPosition Journal::append_record(RecordType type, std::uint8_t flags, ByteView extra,
                                   ByteView payload, bool may_use_reserve) {
  std::uint64_t size = record_size(extra.size(), payload.size());
  ensure_room(size, may_use_reserve);
  Bytes record = encode_record(type, flags, writer_->seq(), extra, payload);
  std::uint64_t at = *writer_->append(record);
  bytes_since_checkpoint_ += size;
  return {writer_->index(), writer_->seq(), at};
}

ChunkRef Journal::append_chunk(const EncodedChunk& chunk, InodeId ino, std::uint64_t file_offset) {
  std::uint64_t size = record_size(kDataChunkHeaderSize - kRecordHeaderSize, chunk.payload.size());
  ensure_room(size, false);
  Bytes record = encode_data_chunk_record(writer_->seq(), chunk, ino, file_offset);
  std::uint64_t at = *writer_->append(record);
  bytes_since_checkpoint_ += size;
  return {at, static_cast<std::uint32_t>(size), chunk.uncompressed_size};
}

LogPosition Journal::append_transaction(std::uint64_t txid, const std::vector<Op>& ops) {
  Encoder out;
  for (const Op& op : ops) encode_op(out, op);
  if (out.size() > max_transaction_payload()) {
    throw Error(Errc::invalid_argument, "transaction too large for a segment");
  }
  std::uint8_t extra[kTransactionExtraSize] = {};
  store_le(extra, txid);
  store_le(extra + 8, static_cast<std::uint32_t>(ops.size()));
  // Transactions may use the reserve: they are small, and refusing one after
  // its changes are already live in memory would be worse.
  return append_record(RecordType::metadata_log, 0, ByteView(extra), out.buffer(), true);
}

LogPosition Journal::append_checkpoint(const Metadata& meta) {
  Bytes stream = meta.encode_checkpoint();
  std::uint64_t hash = xxh3_64(stream);
  std::uint64_t overhead = record_size(kCheckpointExtraSize, 0) + kRecordAlignment;
  std::optional<LogPosition> first;
  std::uint64_t done = 0;
  std::uint32_t part = 0;
  do {
    std::uint64_t room = current_remaining();
    std::uint64_t left = stream.size() - done;
    // Start a new segment rather than write a tiny part into a nearly full one.
    if (room < overhead + std::min(left, kMinCheckpointPart)) {
      roll_over(true);
      room = current_remaining();
    }
    std::uint64_t n = std::min({left, kMaxCheckpointPart, room - overhead});
    std::uint8_t extra[kCheckpointExtraSize] = {};
    store_le(extra, meta.last_txid);
    store_le(extra + 8, part);
    store_le<std::uint32_t>(extra + 12, done + n == stream.size() ? checkpoint_flags::final_part : 0);
    store_le<std::uint64_t>(extra + 16, stream.size());
    store_le(extra + 24, hash);
    LogPosition at = append_record(RecordType::checkpoint, 0, ByteView(extra),
                                   ByteView(stream.data() + done, n), true);
    if (!first) first = at;
    done += n;
    ++part;
  } while (done < stream.size());
  return *first;
}

void Journal::write_buffered() { writer_->write_buffered(); }

void Journal::flush() { writer_->flush(); }

void Journal::checkpoint_done(const LogPosition& start) {
  pending_free_.clear();
  for (auto it = meta_.segments.begin(); it != meta_.segments.end();) {
    const auto& [index, usage] = *it;
    if (usage.live_bytes == 0 && usage.seq < start.seq && index != writer_->index()) {
      pending_free_.insert(index);
      it = meta_.segments.erase(it);
    } else {
      ++it;
    }
  }
  bytes_since_checkpoint_ = 0;
}

void Journal::release_after_mount(const LogPosition& start, std::uint64_t log_end_index) {
  for (auto it = meta_.segments.begin(); it != meta_.segments.end();) {
    const auto& [index, usage] = *it;
    if (usage.live_bytes == 0 && usage.seq < start.seq && index != log_end_index) {
      it = meta_.segments.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- replay ----------------------------------------------------------------

namespace {

// Reads records in log order, following seals across segments.
class LogReader {
 public:
  LogReader(Store& store, const Superblock& sb, LogPosition start)
      : store_(store), sb_(sb), pos_(start) {
    scanner_.emplace(store, sb, start.index, start.offset);
    if (!scanner_->valid() || scanner_->header().seq != start.seq) {
      corrupt("the segment holding the checkpoint is missing");
    }
    visited_.push_back({start.index, start.seq});
  }

  std::optional<Record> next() {
    while (true) {
      std::optional<Record> r = scanner_->next();
      if (!r) {
        pos_.offset = scanner_->end_offset();
        return std::nullopt;
      }
      if (r->header.type != RecordType::segment_seal) {
        bytes_ += r->stored_size();
        pos_.offset = r->offset + r->stored_size();
        return r;
      }
      if (r->extra.size() != kSealExtraSize) corrupt("bad segment seal");
      std::uint64_t index = load_le<std::uint64_t>(r->extra.data());
      std::uint64_t seq = load_le<std::uint64_t>(r->extra.data() + 8);
      std::optional<SegmentHeader> header;
      if (index < sb_.segment_count) header = read_segment_header(store_, sb_, index);
      if (!header || header->seq != seq) {
        // The seal reached the disk but the next segment's header did not:
        // the log ends here, at the seal.
        pos_.offset = r->offset;
        return std::nullopt;
      }
      scanner_.emplace(store_, sb_, index);
      pos_ = {index, seq, sb_.segment_offset(index) + kSegmentHeaderSize};
      visited_.push_back({index, seq});
    }
  }

  const LogPosition& position() const { return pos_; }
  const std::vector<std::pair<std::uint64_t, std::uint64_t>>& visited() const { return visited_; }
  std::uint64_t bytes() const { return bytes_; }

 private:
  Store& store_;
  const Superblock& sb_;
  LogPosition pos_;
  std::optional<SegmentScanner> scanner_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> visited_;
  std::uint64_t bytes_ = 0;
};

void add_visited(Metadata& meta, const LogReader& reader) {
  for (const auto& [index, seq] : reader.visited()) {
    auto [it, inserted] = meta.segments.try_emplace(index, SegmentUsage{seq, 0});
    if (inserted || it->second.seq == seq) continue;
    // A segment the checkpoint still listed, but without live data, may have
    // been freed at mount and reused by the log since (with a newer number).
    if (it->second.live_bytes == 0 && seq > it->second.seq) {
      it->second.seq = seq;
      continue;
    }
    corrupt("segment table disagrees with the log");
  }
}

}  // namespace

ReplayResult replay(Store& store, const Superblock& sb) {
  if (sb.checkpoint_offset == 0) {
    corrupt("no checkpoint (the filesystem was made by an older mkfs.nanoer)");
  }
  // Sequence numbers must only ever grow, including past segments that were
  // started but never linked into the log, so look at every header.
  std::uint64_t max_seq = 0;
  for (std::uint64_t i = 0; i < sb.segment_count; ++i) {
    if (auto h = read_segment_header(store, sb, i)) max_seq = std::max(max_seq, h->seq);
  }

  LogPosition start{(sb.checkpoint_offset - sb.data_start) >> sb.segment_shift, sb.checkpoint_seq,
                    sb.checkpoint_offset};
  LogReader reader(store, sb, start);

  // The checkpoint: parts 0..n in order, the last flagged final.
  Bytes stream;
  std::uint64_t txid = 0, total = 0, hash = 0;
  for (std::uint32_t part = 0;; ++part) {
    std::optional<Record> r = reader.next();
    if (!r || r->header.type != RecordType::checkpoint || r->extra.size() != kCheckpointExtraSize) {
      corrupt("checkpoint is incomplete");
    }
    const std::uint8_t* e = r->extra.data();
    if (load_le<std::uint32_t>(e + 8) != part) corrupt("checkpoint parts out of order");
    if (part == 0) {
      txid = load_le<std::uint64_t>(e);
      total = load_le<std::uint64_t>(e + 16);
      hash = load_le<std::uint64_t>(e + 24);
    } else if (load_le<std::uint64_t>(e) != txid || load_le<std::uint64_t>(e + 16) != total ||
               load_le<std::uint64_t>(e + 24) != hash) {
      corrupt("checkpoint parts do not belong together");
    }
    stream.insert(stream.end(), r->payload.begin(), r->payload.end());
    if (stream.size() > total) corrupt("checkpoint longer than recorded");
    if (load_le<std::uint32_t>(e + 12) & checkpoint_flags::final_part) break;
  }
  if (stream.size() != total || xxh3_64(stream) != hash) corrupt("checkpoint checksum mismatch");

  ReplayResult result{Metadata::decode_checkpoint(stream, sb.data_start, sb.segment_shift),
                      start, {}, max_seq, 0, 0, stream.size()};
  Metadata& meta = result.meta;
  if (meta.last_txid != txid) corrupt("checkpoint transaction id mismatch");
  std::uint64_t checkpoint_log_bytes = reader.bytes();

  // The transactions after it.
  while (std::optional<Record> r = reader.next()) {
    switch (r->header.type) {
      case RecordType::data_chunk:
        break;  // referenced by transactions, not replayed on its own
      case RecordType::checkpoint:
        break;  // a later checkpoint the superblock never pointed to; its state follows anyway
      case RecordType::metadata_log: {
        if (r->extra.size() != kTransactionExtraSize) corrupt("bad transaction header");
        std::uint64_t id = load_le<std::uint64_t>(r->extra.data());
        std::uint32_t count = load_le<std::uint32_t>(r->extra.data() + 8);
        if (id != meta.last_txid + 1) corrupt("transaction ids are not consecutive");
        add_visited(meta, reader);  // before applying: chunks may live in new segments
        Decoder in(r->payload);
        for (std::uint32_t i = 0; i < count; ++i) meta.apply(decode_op(in));
        if (!in.done()) corrupt("trailing bytes in a transaction");
        meta.last_txid = id;
        ++result.transactions;
        break;
      }
      default:
        corrupt("unknown record type " + std::to_string(static_cast<int>(r->header.type)));
    }
  }
  add_visited(meta, reader);
  result.end = reader.position();
  result.log_bytes = reader.bytes() - checkpoint_log_bytes;
  return result;
}

}  // namespace nanoer
