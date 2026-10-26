#include "nanoer/gc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

#include "nanoer/fs.h"
#include "nanoer/io.h"
#include "nanoer/segment.h"

namespace nanoer {

std::vector<GcVictim> choose_victims(const Metadata& meta, std::uint64_t usable,
                                     std::uint64_t exclude, std::uint64_t newest_seq,
                                     const GcPolicy& policy) {
  struct Scored {
    GcVictim victim;
    double score;
  };
  std::vector<Scored> scored;
  for (const auto& [index, usage] : meta.segments) {
    // Empty segments need no copying: the next two checkpoints free them.
    if (index == exclude || usage.live_bytes == 0) continue;
    double u = static_cast<double>(usage.live_bytes) / static_cast<double>(usable);
    if (u > policy.max_utilization) continue;
    double age = static_cast<double>(newest_seq > usage.seq ? newest_seq - usage.seq : 0) + 1;
    double score = policy.greedy ? 1 - u : (1 - u) * age / (1 + u);
    scored.push_back({{index, usage.seq, usage.live_bytes}, score});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    return a.score != b.score ? a.score > b.score : a.victim.index < b.victim.index;
  });
  std::vector<GcVictim> out;
  out.reserve(scored.size());
  for (const Scored& s : scored) out.push_back(s.victim);
  return out;
}

// ---- the filesystem's side --------------------------------------------------
//
// Moving a record is an ordinary write: the copy is appended to the log, then
// a transaction re-points every chunk that still refers to the old location.
// Replay stops at the first record that is not intact, so a crash leaves
// either the old location in use or the new one, and both are intact (the old
// one stays until two checkpoints later, docs/format.md §10.4).

namespace {

// How much live data one step moves: bounds how long the lock is held.
constexpr std::uint64_t kGcStepBytes = 8 << 20;
// Background GC: when the filesystem has had no changes for a while
// (Options::gc_idle_s) and this much of the space in use is dead; or when
// free segments run low.
constexpr std::uint64_t kGcIdleDeadPercent = 25;
constexpr std::uint64_t kGcLowSpacePercent = 15;

std::int64_t now() { return static_cast<std::int64_t>(now_ns()); }

}  // namespace

Filesystem::GcNeed Filesystem::gc_need() const {
  if (read_only_ || failed_ || !journal_) return GcNeed::none;
  std::uint64_t low = std::max(journal_->reserve() + 2, sb_.segment_count * kGcLowSpacePercent / 100);
  if (journal_->free_segments() < low) return GcNeed::low_space;
  if (now() - last_change_ns_ < std::int64_t{options_.gc_idle_s} * 1'000'000'000 || !dirty_.empty() ||
      !pending_ops_.empty()) {
    return GcNeed::none;
  }
  std::uint64_t usable = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  std::uint64_t used = meta_.segments.size() * usable, live = 0;
  for (const auto& [index, usage] : meta_.segments) live += usage.live_bytes;
  return (used - std::min(used, live)) * 100 > used * kGcIdleDeadPercent ? GcNeed::idle : GcNeed::none;
}

Filesystem::GcPlan Filesystem::gc_plan(const GcPolicy& policy, bool urgent) const {
  GcPlan plan;
  std::uint64_t usable = sb_.segment_size() - kSegmentHeaderSize - kSealRecordSize;
  // No more than the copies have room for: a victim half copied frees nothing.
  const std::uint64_t budget = std::min(kGcStepBytes, journal_->copy_room(urgent));
  std::uint64_t planned = 0;
  for (const GcVictim& v : choose_victims(meta_, usable, journal_->current_index(), journal_->next_seq(), policy)) {
    if (gc_skip_.contains({v.index, v.seq})) continue;
    if (planned + v.live_bytes > budget) break;
    plan.victims.push_back(v);
    planned += v.live_bytes;
  }
  if (plan.victims.empty()) return plan;
  plan.codec = sb_.default_codec;
  plan.level = sb_.default_level;
  std::map<std::uint64_t, std::uint64_t> seq_of;
  for (const GcVictim& v : plan.victims) seq_of.emplace(v.index, v.seq);

  // Everything that refers into the victims, by record.
  std::map<std::uint64_t, GcItem> items;
  for (const auto& [id, inode] : meta_.inodes) {
    for (const auto& [index, ref] : inode.chunks) {
      auto victim = seq_of.find(meta_.segment_of(ref.offset));
      if (victim == seq_of.end()) continue;
      std::uint64_t limit = sb_.segment_offset(victim->first) + sb_.segment_size();
      GcItem& item =
          items.try_emplace(ref.offset, GcItem{ref.offset, victim->second, limit, ref.stored_size, {}}).first->second;
      item.users.push_back({id, index, ref});
    }
  }
  std::uint64_t taken = 0;
  for (auto& [offset, item] : items) {
    if (taken + item.stored_size > budget) break;
    taken += item.stored_size;
    plan.items.push_back(std::move(item));
  }
  return plan;
}

std::vector<Filesystem::GcRead> Filesystem::gc_read(const GcPlan& plan) {
  std::vector<GcRead> out;
  out.reserve(plan.items.size());
  for (const GcItem& item : plan.items) {
    GcRead read;
    std::optional<Record>& r = read.record;
    try {
      r = try_read_record(store_, item.offset, item.limit, item.seq);
      // The record must be what the references say it is (its CRCs were
      // checked by reading it).
      if (r && (r->header.type != RecordType::data_chunk || r->stored_size() != item.stored_size)) r.reset();
      if (r) {
        DataChunkInfo info = parse_data_chunk(*r);
        for (const GcItem::User& u : item.users) {
          bool fits = u.ref.packed ? std::uint64_t{u.ref.piece} + u.ref.size <= info.uncompressed_size
                                   : info.uncompressed_size == u.ref.size;
          if (!fits || info.pack != u.ref.packed) r.reset();
        }
        // A pack mostly dead (its files changed or deleted): keep only the
        // live pieces, packed again.
        if (r && info.pack) {
          std::map<std::uint32_t, std::uint32_t> live;  // piece -> size
          for (const GcItem::User& u : item.users) live.emplace(u.ref.piece, u.ref.size);
          std::uint64_t live_bytes = 0;
          for (const auto& [piece, size] : live) live_bytes += size;
          if (live_bytes * 2 < info.uncompressed_size) {
            Bytes old = decode_data_chunk(*r);
            Bytes fresh;
            for (const auto& [piece, size] : live) {
              read.pieces.emplace(piece, static_cast<std::uint32_t>(fresh.size()));
              fresh.insert(fresh.end(), old.begin() + piece, old.begin() + piece + size);
            }
            read.repacked = encode_chunk(fresh, plan.codec, plan.level);
          }
        }
      }
    } catch (const Error&) {
      r.reset();  // unreadable: decided under the lock (the segment may have been reused)
      read.repacked.reset();
    }
    out.push_back(std::move(read));
  }
  return out;
}

bool Filesystem::gc_apply(const GcPlan& plan, std::vector<GcRead>& reads, bool urgent) {
  if (read_only_ || failed_ || !journal_) return false;
  // A victim freed and reused while its records were read: they are stale.
  for (const GcVictim& v : plan.victims) {
    auto it = meta_.segments.find(v.index);
    if (it == meta_.segments.end() || it->second.seq != v.seq) return false;
  }
  struct Flag {
    bool& flag;
    explicit Flag(bool& f) : flag(f) { flag = true; }
    ~Flag() { flag = false; }
  } in_gc(in_gc_);

  bool moved = false;
  for (std::size_t i = 0; i < plan.items.size(); ++i) {
    const GcItem& item = plan.items[i];
    // Only chunks that still refer to the record move with it.
    std::vector<GcItem::User> users;
    for (const GcItem::User& u : item.users) {
      const Inode* file = meta_.find(u.ino);
      if (!file) continue;
      auto c = file->chunks.find(u.index);
      if (c != file->chunks.end() && c->second == u.ref) users.push_back(u);
    }
    if (users.empty()) continue;
    if (!reads[i].record) {
      // Damaged data stays where it is (reads report it); the segment is
      // not collected again this mount.
      ++gc_stats_.damaged;
      errors_seen_ = true;
      std::uint64_t segment = meta_.segment_of(item.offset);
      gc_skip_.insert({segment, item.seq});
      std::fprintf(stderr, "nanoer: garbage collection found damaged data at byte %llu (inode %llu); left in place\n",
                   static_cast<unsigned long long>(item.offset),
                   static_cast<unsigned long long>(users.front().ino));
      continue;
    }
    ChunkRef copy;
    try {
      if (reads[i].repacked) {
        copy = journal_->append_repacked(*reads[i].repacked, urgent);
        ++gc_stats_.packs_repacked;
      } else {
        copy.offset = journal_->append_copy(*reads[i].record, urgent);
        copy.stored_size = item.stored_size;
      }
    } catch (const Error& e) {
      if (e.code() == Errc::no_space) break;  // nothing was written
      throw;
    }
    for (std::size_t k = 0; k < users.size(); ++k) {
      ChunkRef ref = users[k].ref;
      ref.offset = copy.offset;
      ref.stored_size = copy.stored_size;
      if (reads[i].repacked) ref.piece = reads[i].pieces.at(ref.piece);
      if (!ref.packed && !ref.hash) {
        record(op::SetChunk{users[k].ino, users[k].index, ref});
      } else {
        // The first reference claims the copy; the others share it.
        record(op::SetChunkRef{users[k].ino, users[k].index, ref, !ref.packed && k > 0});
      }
    }
    ++gc_stats_.records_copied;
    gc_stats_.bytes_copied += copy.stored_size;
    moved = true;
  }
  if (moved) commit();
  ++gc_stats_.steps;
  for (const GcVictim& v : plan.victims) {
    auto it = meta_.segments.find(v.index);
    if (it != meta_.segments.end() && it->second.seq == v.seq && it->second.live_bytes == 0) {
      ++gc_stats_.segments_emptied;
      ++gc_emptied_;
    }
  }
  return moved;
}

bool Filesystem::gc_step_locked(const GcPolicy& policy, bool urgent) {
  GcPlan plan = gc_plan(policy, urgent);
  if (plan.items.empty()) return false;
  std::vector<GcRead> reads = gc_read(plan);
  return gc_apply(plan, reads, urgent);
}

void Filesystem::gc_checkpoints(bool low_space) {
  // Emptied segments are released by one checkpoint and free after the
  // next; checkpoints rewrite all metadata (until M6), so wait for several.
  std::uint64_t worth = low_space ? 1 : std::max<std::uint64_t>(2, sb_.segment_count / 20);
  if (gc_emptied_ < worth || read_only_ || failed_ || !journal_) return;
  checkpoint();
  checkpoint();
  gc_emptied_ = 0;
}

void Filesystem::background_gc() {
  GcNeed need;
  {
    std::shared_lock lock(mutex_);
    need = gc_need();
  }
  if (need == GcNeed::none) return;
  // Idle: the segments whose copying gains the most. Low on space: the
  // emptiest, but not nearly full ones (copying those gains little).
  const GcPolicy policy = need == GcNeed::idle ? GcPolicy{.greedy = false, .max_utilization = 0.75}
                                               : GcPolicy{.greedy = true, .max_utilization = 0.8};
  for (int step = 0; step < (need == GcNeed::idle ? 2 : 8); ++step) {
    GcPlan plan;
    {
      std::shared_lock lock(mutex_);
      if (gc_need() == GcNeed::none) break;
      plan = gc_plan(policy, false);
    }
    if (plan.items.empty()) break;
    std::vector<GcRead> reads = gc_read(plan);
    std::unique_lock lock(mutex_);
    if (!gc_apply(plan, reads, false)) break;
  }
  std::unique_lock lock(mutex_);
  gc_checkpoints(need == GcNeed::low_space);
}

std::uint64_t Filesystem::collect_garbage(double max_utilization) {
  std::unique_lock lock(mutex_);
  check_writable();
  commit();
  std::uint64_t before = journal_->free_segments();
  const GcPolicy policy{.greedy = true, .max_utilization = std::min(max_utilization, 0.97)};
  while (gc_step_locked(policy, false)) {
  }
  checkpoint();
  checkpoint();
  gc_emptied_ = 0;
  std::uint64_t after = journal_->free_segments();
  return after > before ? after - before : 0;
}

}  // namespace nanoer
