// Tests: reaching into a Filesystem (it names this struct as a friend).
#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include "nanoer/fs.h"

namespace nanoer {

// GC one phase at a time, so a test can interleave other work; and the
// metadata, including the dedup index.
struct FilesystemTestAccess {
  static Filesystem::GcPlan plan(Filesystem& fs, double max_utilization) {
    return fs.gc_plan({.greedy = true, .max_utilization = max_utilization}, false);
  }
  static std::vector<std::optional<Record>> read(Filesystem& fs, const Filesystem::GcPlan& plan) {
    return fs.gc_read(plan);
  }
  static bool apply(Filesystem& fs, const Filesystem::GcPlan& plan, std::vector<std::optional<Record>>& r) {
    std::unique_lock lock(fs.mutex_);
    return fs.gc_apply(plan, r, false);
  }
  static void set_index(Filesystem& fs, std::uint64_t key, const DedupTarget& target) {
    std::unique_lock lock(fs.mutex_);
    fs.meta_.dedup.put(key, target);
  }
  static const Metadata& metadata(Filesystem& fs) { return fs.meta_; }
};

}  // namespace nanoer
