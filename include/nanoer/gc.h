// Garbage collection: copying the live data out of segments that are mostly
// dead, so that whole segments can be reused (docs/M4_PLAN.md §3,
// KNOWLEDGE.md §7.6). The filesystem drives it; this is the policy.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nanoer/metadata.h"

namespace nanoer {

struct GcVictim {
  std::uint64_t index = 0;  // segment
  std::uint64_t seq = 0;    // its sequence number when chosen
  std::uint64_t live_bytes = 0;
};

struct GcPolicy {
  // Least live data first: when space is urgent. Otherwise cost-benefit, as
  // in Sprite LFS: (1 - u) * age / (1 + u), so old data, which tends to stay,
  // is collected even from fuller segments.
  bool greedy = false;
  double max_utilization = 0.75;  // fuller segments are not worth copying
};

// Segments worth collecting, best first. `usable`: the data bytes a segment
// holds; `exclude`: the segment being written; `newest_seq`: the newest
// sequence number (ages are measured from it).
std::vector<GcVictim> choose_victims(const Metadata& meta, std::uint64_t usable,
                                     std::uint64_t exclude, std::uint64_t newest_seq,
                                     const GcPolicy& policy);

struct GcStats {
  std::uint64_t steps = 0;
  std::uint64_t records_copied = 0;
  std::uint64_t bytes_copied = 0;      // stored bytes
  std::uint64_t segments_emptied = 0;  // left with no live data (tree nodes leave at the next checkpoint)
  std::uint64_t damaged = 0;           // records that could not be read: left in place
  std::uint64_t packs_repacked = 0;    // packs mostly dead: their live pieces packed again
};

}  // namespace nanoer
