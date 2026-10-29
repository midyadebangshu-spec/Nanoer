// Tests: reaching into a Filesystem (it names this struct as a friend).
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

#include "nanoer/fs.h"

namespace nanoer {

// GC one phase at a time, so a test can interleave other work; and the
// metadata, including the dedup index.
struct FilesystemTestAccess {
  static Filesystem::GcPlan plan(Filesystem& fs, double max_utilization) {
    return fs.gc_plan({.greedy = true, .max_utilization = max_utilization}, false);
  }
  static std::vector<Filesystem::GcRead> read(Filesystem& fs, const Filesystem::GcPlan& plan) {
    return fs.gc_read(plan);
  }
  static bool apply(Filesystem& fs, const Filesystem::GcPlan& plan, std::vector<Filesystem::GcRead>& r) {
    std::unique_lock lock(fs.mutex_);
    return fs.gc_apply(plan, r, false);
  }
  static void set_index(Filesystem& fs, std::uint64_t key, const DedupTarget& target) {
    std::unique_lock lock(fs.mutex_);
    fs.meta_.dedup.put(key, target);
  }
  static const Metadata& metadata(Filesystem& fs) { return fs.meta_; }
  static void checkpoint(Filesystem& fs) {
    std::unique_lock lock(fs.mutex_);
    fs.commit();
    fs.checkpoint();
  }
  // A file's committed chunks.
  static std::map<std::uint64_t, ChunkRef> chunks(Filesystem& fs, InodeId ino) {
    std::shared_lock lock(fs.mutex_);
    std::map<std::uint64_t, ChunkRef> out;
    fs.meta_.chunks(ino, 0, [&](std::uint64_t index, const ChunkRef& ref) {
      out.emplace(index, ref);
      return true;
    });
    return out;
  }

  // Tiering one phase at a time: a pass over the files cold after `age_s`.
  static Filesystem::TierPlan tier_plan(Filesystem& fs, std::int64_t age_s) {
    std::lock_guard pass(fs.tier_mutex_);
    std::shared_lock lock(fs.mutex_);
    std::int64_t t = static_cast<std::int64_t>(now_ns());
    fs.tier_begin_pass(t, age_s);
    return fs.tier_plan(std::uint64_t{1} << 40, t, age_s);
  }
  static std::vector<Filesystem::TierRead> tier_read(Filesystem& fs, const Filesystem::TierPlan& plan) {
    return fs.tier_read(plan);
  }
  static std::uint64_t tier_apply(Filesystem& fs, const Filesystem::TierPlan& plan,
                                  std::vector<Filesystem::TierRead>& r) {
    std::unique_lock lock(fs.mutex_);
    return fs.tier_apply(plan, r);
  }
};

}  // namespace nanoer
