// Creating a new filesystem on a store.
#pragma once

#include <optional>
#include <string>

#include "nanoer/codec.h"
#include "nanoer/format.h"

namespace nanoer {

class Store;

struct FormatOptions {
  unsigned segment_shift = kDefaultSegmentShift;
  unsigned chunk_shift = kDefaultChunkShift;
  CodecId codec = CodecId::zstd;
  int level = 1;
  std::string label;
  std::optional<Uuid> uuid;  // random when not set
  std::uint32_t root_uid = 0;  // owner of the root directory
  std::uint32_t root_gid = 0;
  // Shared records (dedup, small-file packs). Off only to make a filesystem
  // as M3 made them, for compatibility tests.
  bool shared_records = true;
  bool cold_data = true;  // cold data recompression (needs shared_records)
  bool metadata_tree = true;  // false: metadata in full checkpoints, as M2-M5 made them (tests)
  bool dedup = true;  // false: the no_dedup preference
};

// The smallest store that can hold a filesystem with this segment size.
std::uint64_t min_store_size(unsigned segment_shift);

// Writes an empty filesystem: clears the reserved area at the start of the
// store, writes a first checkpoint (an empty root directory) into segment 0,
// and writes both superblocks. Other segments are not touched; they count as
// free because their headers carry another (or no) filesystem UUID. Returns
// the newest superblock. Throws Error(invalid_argument) for bad options.
Superblock make_filesystem(Store& store, const FormatOptions& options);

}  // namespace nanoer
