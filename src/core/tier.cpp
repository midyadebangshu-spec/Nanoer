#include "nanoer/tier.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>

#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/segment.h"

namespace nanoer {

// Recompressing cold data is an ordinary write, as GC's copies are: the new
// records are appended to the log, then a transaction re-points every chunk
// that still refers to what was read. A crash leaves either the old records
// in use or the new ones, and the old ones stay intact until two checkpoints
// later (docs/format.md §10.4).

namespace {

// Uncompressed data one step takes: bounds how long a step's re-pointing
// holds the lock, and how much is written before space is checked again.
constexpr std::uint64_t kTierStepBytes = 16 << 20;
constexpr std::uint64_t kTierCallStepBytes = 64 << 20;  // recompress_cold
// A background pass runs while the filesystem is idle, for at most this
// long at a time, and a new one starts once something changed or files
// may have aged into coldness.
constexpr auto kTierSlice = std::chrono::seconds(2);
constexpr std::int64_t kTierRescanNs = std::int64_t{600} * 1'000'000'000;
// Recompressed data must come out at least this much smaller (percent) to
// be written; otherwise it is only marked cold.
constexpr std::uint64_t kTierMinGainPercent = 3;
// Background tiering leaves free at least what background GC aims for.
constexpr std::uint64_t kTierLowSpacePercent = 15;

std::int64_t now() { return static_cast<std::int64_t>(now_ns()); }

std::uint64_t stored_size_of(const EncodedChunk& chunk, bool with_id) {
  std::size_t header = with_id ? kDataChunkHeaderSizeWithId : kDataChunkHeaderSize;
  return align_up(std::uint64_t{header} + chunk.payload.size(), kRecordAlignment);
}

}  // namespace

bool Filesystem::cold_enabled() const {
  return has_cold_data(sb_) && !read_only_ && !failed_ && journal_ != nullptr;
}

std::optional<StorageForm> Filesystem::cold_now(const Inode& file, std::int64_t now_ns,
                                                std::optional<std::int64_t> age_s) const {
  if (!file.is_regular() || file.chunks.empty()) return std::nullopt;
  Policy policy = policy_of(file);
  std::optional<std::int64_t> after = cold_after_s(policy, age_s.value_or(options_.cold_after_s));
  if (!after) return std::nullopt;
  // The change time cannot be set by users (unlike mtime, which tar and
  // cp -p copy), so it says when the file was last written.
  std::int64_t changed = std::max(file.ctime_ns, file.mtime_ns);
  if (now_ns - changed < *after * 1'000'000'000) return std::nullopt;
  if (dirty_.contains(file.id)) return std::nullopt;
  return cold_form(policy);
}

bool Filesystem::tier_eligible(const TierMember& m) const {
  const Inode* file = meta_.find(m.ino);
  if (!file) return false;
  auto c = file->chunks.find(m.index);
  if (c == file->chunks.end() || c->second != m.ref) return false;
  auto usage = meta_.segments.find(meta_.segment_of(m.ref.offset));
  return usage != meta_.segments.end() && usage->second.seq == m.seq;
}

void Filesystem::tier_begin_pass(std::int64_t now_ns, std::optional<std::int64_t> age_s) {
  tier_queue_.clear();
  tier_shared_.clear();
  // Records with several references (packs, deduplicated data) are taken
  // only when every reference is from a cold file: one still changing would
  // soon leave a fresh record behind anyway.
  struct Shared {
    std::vector<TierMember> members;
    bool all_cold = true;  // every user is a cold file
    bool any_hot = false;  // some reference is not marked cold yet
    bool packed = false;
    StorageForm form{};
  };
  std::map<std::uint64_t, Shared> shared;
  std::vector<InodeId> files;
  for (const auto& [id, inode] : meta_.inodes) {
    if (!inode.is_regular()) continue;
    std::optional<StorageForm> form = cold_now(inode, now_ns, age_s);
    bool candidate = false;
    for (const auto& [index, ref] : inode.chunks) {
      auto usage = meta_.segments.find(meta_.segment_of(ref.offset));
      if (usage == meta_.segments.end()) continue;
      if (tier_skip_.contains({ref.offset, usage->second.seq})) continue;
      if (!ref.packed && meta_.references(ref) == 1) {
        candidate |= form && !ref.cold;
        continue;
      }
      Shared& s = shared[ref.offset];
      if (s.members.empty()) {
        s.packed = ref.packed;
        if (form) s.form = *form;
      }
      std::uint64_t limit = sb_.segment_offset(usage->first) + sb_.segment_size();
      s.members.push_back({id, index, ref, usage->second.seq, limit});
      s.all_cold &= form.has_value();
      s.any_hot |= !ref.cold;
    }
    if (candidate) files.push_back(id);
  }
  std::sort(files.rbegin(), files.rend());  // taken from the back: lowest inode first
  tier_queue_ = std::move(files);
  for (auto it = shared.rbegin(); it != shared.rend(); ++it) {
    Shared& s = it->second;
    if (!s.all_cold || !s.any_hot) continue;
    tier_shared_.push_back({s.packed ? TierItem::Kind::pack : TierItem::Kind::single, std::move(s.members), s.form});
  }
  tier_pass_open_ = true;
  tier_pass_ns_ = now_ns;
  // (tier_stats_ is counted by the caller, under the unique lock: readers
  // of info() hold the shared one.)
}

Filesystem::TierPlan Filesystem::tier_plan(std::uint64_t budget, std::int64_t now_ns,
                                           std::optional<std::int64_t> age_s,
                                           const std::vector<InodeId>* only) {
  TierPlan plan;

  std::size_t next_only = 0;
  auto next_file = [&]() -> std::optional<InodeId> {
    if (only) {
      if (next_only == only->size()) return std::nullopt;
      return (*only)[next_only++];
    }
    if (tier_queue_.empty()) return std::nullopt;
    InodeId id = tier_queue_.back();
    tier_queue_.pop_back();
    return id;
  };
  while (plan.bytes < budget) {
    std::optional<InodeId> id = next_file();
    if (!id) break;
    const Inode* file = meta_.find(*id);
    if (!file) continue;
    std::optional<StorageForm> form = cold_now(*file, now_ns, age_s);
    if (!form) continue;
    // The file's chunks that only it refers to and that are not cold yet,
    // each recompressed on its own (KNOWLEDGE.md §7.7: chunks compressed
    // together in larger units read 25-30 % slower).
    for (const auto& [index, ref] : file->chunks) {
      if (ref.cold || ref.packed || meta_.references(ref) != 1) continue;
      auto usage = meta_.segments.find(meta_.segment_of(ref.offset));
      if (usage == meta_.segments.end() || tier_skip_.contains({ref.offset, usage->second.seq})) continue;
      if (plan.bytes > 0 && plan.bytes + ref.size > budget) {
        // The rest of a large file waits for the next step.
        if (!only) tier_queue_.push_back(*id);
        return plan;
      }
      std::uint64_t limit = sb_.segment_offset(usage->first) + sb_.segment_size();
      plan.items.push_back({TierItem::Kind::single, {{*id, index, ref, usage->second.seq, limit}}, *form});
      plan.bytes += ref.size;
    }
  }
  if (only) return plan;
  while (plan.bytes < budget && !tier_shared_.empty()) {
    TierItem item = std::move(tier_shared_.back());
    tier_shared_.pop_back();
    // The data is read once, whatever the number of references.
    if (item.kind == TierItem::Kind::pack) {
      plan.bytes += sb_.chunk_size();
    } else {
      plan.bytes += item.members.front().ref.size;
    }
    plan.items.push_back(std::move(item));
  }
  return plan;
}

std::vector<Filesystem::TierRead> Filesystem::tier_read(const TierPlan& plan) {
  std::vector<TierRead> out(plan.items.size());
  auto read_one = [&](std::size_t i) {
    const TierItem& item = plan.items[i];
    TierRead& r = out[i];
    try {
      auto read = [&](const TierMember& m) -> std::optional<Record> {
        std::optional<Record> rec = try_read_record(store_, m.ref.offset, m.limit, m.seq);
        if (rec && (rec->header.type != RecordType::data_chunk || rec->stored_size() != m.ref.stored_size)) {
          rec.reset();
        }
        return rec;
      };
      {
        const TierMember& m = item.members.front();
        std::optional<Record> rec = read(m);
        if (!rec) throw Error(Errc::corrupt, "unreadable");
        DataChunkInfo info = parse_data_chunk(*rec);
        bool pack = item.kind == TierItem::Kind::pack;
        if (info.pack != pack) throw Error(Errc::corrupt, "not as referenced");
        for (const TierMember& u : item.members) {
          bool fits = pack ? std::uint64_t{u.ref.piece} + u.ref.size <= info.uncompressed_size
                           : info.uncompressed_size == u.ref.size;
          if (!fits || u.ref.offset != m.ref.offset) throw Error(Errc::corrupt, "not as referenced");
        }
        Bytes data = decode_data_chunk(*rec);
        if (pack) {
          // Only the live pieces, in their order.
          std::map<std::uint32_t, std::uint32_t> live;  // piece -> size
          for (const TierMember& u : item.members) {
            auto [it, first] = live.emplace(u.ref.piece, u.ref.size);
            if (!first) it->second = std::max(it->second, u.ref.size);
          }
          Bytes fresh;
          for (const auto& [piece, size] : live) {
            r.moved.emplace(piece, static_cast<std::uint32_t>(fresh.size()));
            fresh.insert(fresh.end(), data.begin() + piece, data.begin() + piece + size);
          }
          data = std::move(fresh);
        } else {
          r.owner = info.inode;
          r.file_offset = info.file_offset;
          r.blake3 = info.blake3;
        }
        r.stored_before = m.ref.stored_size;
        r.encoded = encode_chunk(data, item.form.codec, item.form.level);
        r.in_place =
            stored_size_of(r.encoded, r.blake3.has_value()) * 100 > r.stored_before * (100 - kTierMinGainPercent);
      }
      r.ok = true;
    } catch (const Error&) {
      // Unreadable: decided under the lock (the segment may have been reused).
      r = TierRead{};
    }
  };
  if (pool_) {
    pool_->parallel_for(plan.items.size(), read_one);
  } else {
    for (std::size_t i = 0; i < plan.items.size(); ++i) read_one(i);
  }
  return out;
}

std::uint64_t Filesystem::tier_apply(const TierPlan& plan, std::vector<TierRead>& reads) {
  if (read_only_ || failed_ || !journal_) return 0;
  struct Flag {
    bool& flag;
    explicit Flag(bool& f) : flag(f) { flag = true; }
    ~Flag() { flag = false; }
  } in_gc(in_gc_);

  std::uint64_t marked = 0;
  bool changed = false;
  std::set<std::uint64_t> old_segments;
  for (std::size_t i = 0; i < plan.items.size(); ++i) {
    const TierItem& item = plan.items[i];
    TierRead& r = reads[i];
    // Only chunks that still refer to what was read are re-pointed.
    std::vector<std::size_t> live;
    for (std::size_t k = 0; k < item.members.size(); ++k) {
      if (tier_eligible(item.members[k])) live.push_back(k);
    }
    if (live.empty()) continue;
    if (!r.ok) {
      const TierMember& m = item.members.front();
      if (!tier_eligible(m)) continue;  // it changed meanwhile: not damage
      // Damaged data stays where it is (reads report it).
      ++tier_stats_.damaged;
      errors_seen_ = true;
      tier_skip_.insert({m.ref.offset, m.seq});
      std::fprintf(stderr, "nanoer: recompressing cold data found damaged data at byte %llu (inode %llu); left in place\n",
                   static_cast<unsigned long long>(m.ref.offset), static_cast<unsigned long long>(m.ino));
      continue;
    }
    for (std::size_t k : live) old_segments.insert(meta_.segment_of(item.members[k].ref.offset));
    tier_stats_.bytes_read += r.encoded.uncompressed_size;
    if (r.in_place) {
      for (std::size_t k : live) {
        const TierMember& m = item.members[k];
        ChunkRef ref = m.ref;
        ref.cold = true;
        record(op::SetChunkRef{m.ino, m.index, ref, !ref.packed && meta_.references(ref) > 1});
      }
      ++tier_stats_.in_place;
      marked += live.size();
      changed = true;
      continue;
    }
    ChunkRef fresh;
    try {
      if (item.kind == TierItem::Kind::pack) {
        fresh = journal_->append_chunk(r.encoded, 0, 0, std::nullopt, true);
      } else {
        fresh = journal_->append_chunk(r.encoded, r.owner, r.file_offset, r.blake3);
      }
    } catch (const Error& e) {
      if (e.code() == Errc::no_space) break;  // nothing was written
      throw;
    }
    for (std::size_t j = 0; j < live.size(); ++j) {
      const TierMember& m = item.members[live[j]];
      ChunkRef ref = m.ref;
      ref.offset = fresh.offset;
      ref.stored_size = fresh.stored_size;
      ref.cold = true;
      bool share = false;
      if (item.kind == TierItem::Kind::pack) {
        ref.piece = r.moved.at(m.ref.piece);
      } else {
        share = j > 0;  // the first claims the record
      }
      record(op::SetChunkRef{m.ino, m.index, ref, share});
    }
    ++(item.kind == TierItem::Kind::pack ? tier_stats_.packs : tier_stats_.singles);
    tier_stats_.stored_before += r.stored_before;
    tier_stats_.stored_after += fresh.stored_size;
    marked += live.size();
    changed = true;
  }
  tier_stats_.chunks += marked;
  ++tier_stats_.steps;
  if (changed) commit();
  for (std::uint64_t segment : old_segments) {
    auto it = meta_.segments.find(segment);
    if (it != meta_.segments.end() && it->second.live_bytes == 0 && segment != journal_->current_index()) {
      ++gc_emptied_;
    }
  }
  return marked;
}

bool Filesystem::tier_room(const TierPlan& plan, bool background) const {
  // What a step writes is at most what it read, plus headers; each new
  // segment may lose up to a chunk at its end.
  const std::uint64_t usable = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  const std::uint64_t tail = sb_.chunk_size() + kMaxRecordHeaderSize;
  std::uint64_t bytes = plan.bytes + plan.items.size() * kMaxRecordHeaderSize;
  std::uint64_t segments = bytes / (usable - tail) + 2;
  std::uint64_t keep = journal_->reserve();
  if (background) keep = std::max(keep + 2, sb_.segment_count * kTierLowSpacePercent / 100);
  return journal_->free_segments() >= keep + segments;
}

std::uint64_t Filesystem::tier_files_locked(const std::vector<InodeId>& files) {
  std::uint64_t marked = 0;
  for (std::size_t from = 0; from < files.size();) {
    std::vector<InodeId> some;
    // A few files at a time; a large one is planned up to the budget, and
    // the rest of it waits for the next step.
    for (; from < files.size() && some.size() < 64; ++from) some.push_back(files[from]);
    TierPlan plan = tier_plan(kTierStepBytes, now(), std::nullopt, &some);
    if (plan.items.empty()) continue;
    if (!tier_room(plan, false)) break;
    std::vector<TierRead> reads = tier_read(plan);
    marked += tier_apply(plan, reads);
  }
  return marked;
}

void Filesystem::background_tier() {
  if (options_.cold_after_s < 0) return;
  std::unique_lock pass(tier_mutex_, std::try_to_lock);
  if (!pass.owns_lock()) return;
  auto idle = [&] {
    return cold_enabled() && now() - last_change_ns_ >= std::int64_t{options_.gc_idle_s} * 1'000'000'000 &&
           dirty_.empty() && pending_ops_.empty();
  };
  bool began = false;
  {
    std::shared_lock lock(mutex_);
    if (!idle()) return;
    if (!tier_pass_open_) {
      std::int64_t t = now();
      bool due = tier_pass_ns_ == 0 || last_change_ns_ > tier_pass_ns_ || t - tier_pass_ns_ >= kTierRescanNs;
      if (!due) return;
      tier_begin_pass(t, std::nullopt);
      began = true;
    }
  }
  if (began) {
    std::unique_lock lock(mutex_);
    ++tier_stats_.passes;
  }
  const auto until = std::chrono::steady_clock::now() + kTierSlice;
  while (std::chrono::steady_clock::now() < until) {
    TierPlan plan;
    {
      std::shared_lock lock(mutex_);
      if (!idle()) break;
      plan = tier_plan(kTierStepBytes, now(), std::nullopt);
      if (plan.items.empty()) {
        tier_pass_open_ = false;
        break;
      }
      if (!tier_room(plan, true)) {
        // Not now: the pass is tried again later, from the start.
        tier_pass_open_ = false;
        break;
      }
    }
    std::vector<TierRead> reads = tier_read(plan);
    std::unique_lock lock(mutex_);
    tier_apply(plan, reads);
  }
  std::unique_lock lock(mutex_);
  gc_checkpoints(false);
}

std::uint64_t Filesystem::recompress_cold(std::optional<std::int64_t> cold_after_s) {
  std::unique_lock pass(tier_mutex_);
  std::unique_lock lock(mutex_);
  check_writable();
  if (!has_cold_data(sb_)) throw Error(Errc::unsupported, "this filesystem predates cold data recompression");
  commit();
  const std::int64_t t = now();
  tier_begin_pass(t, cold_after_s);
  ++tier_stats_.passes;
  std::uint64_t marked = 0;
  while (true) {
    TierPlan plan = tier_plan(kTierCallStepBytes, t, cold_after_s);
    if (plan.items.empty()) break;
    if (!tier_room(plan, false)) {
      // Free what earlier steps left dead, then try once more.
      if (checkpoints_free_space()) {
        checkpoint();
        checkpoint();
        gc_emptied_ = 0;
      }
      if (!tier_room(plan, false)) {
        std::fprintf(stderr, "nanoer: not enough free space to recompress all cold data\n");
        break;
      }
    }
    std::vector<TierRead> reads = tier_read(plan);
    marked += tier_apply(plan, reads);
  }
  tier_pass_open_ = false;
  checkpoint();
  checkpoint();
  gc_emptied_ = 0;
  return marked;
}

}  // namespace nanoer
