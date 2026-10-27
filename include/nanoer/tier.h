// Cold data recompressed (docs/M5_PLAN.md §3, KNOWLEDGE.md §7.7). Data of
// files that have not changed for a while is rewritten in its cold form,
// each chunk on its own with a higher level (shared records once for all
// their users), and packs of small files again. The filesystem drives it
// (tier.cpp); this is what it reports.
#pragma once

#include <cstdint>

namespace nanoer {

struct TierStats {
  std::uint64_t passes = 0;       // sweeps over the whole filesystem
  std::uint64_t steps = 0;
  std::uint64_t chunks = 0;       // chunk references marked cold
  std::uint64_t singles = 0;      // chunk records rewritten (shared ones once for all their users)
  std::uint64_t packs = 0;        // packs of small files rewritten
  std::uint64_t in_place = 0;     // records marked cold as they are: recompressing would not shrink them
  std::uint64_t bytes_read = 0;   // uncompressed data examined
  std::uint64_t stored_before = 0;  // what the rewritten records took
  std::uint64_t stored_after = 0;   // what their cold form takes
  std::uint64_t damaged = 0;      // records that could not be read: left as they are
};

}  // namespace nanoer
