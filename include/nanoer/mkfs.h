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
};

// The smallest store that can hold a filesystem with this segment size.
std::uint64_t min_store_size(unsigned segment_shift);

// Writes an empty filesystem: clears the reserved area at the start of the
// store and writes both superblocks. Segments are not touched; they count as
// free because their headers carry another (or no) filesystem UUID. Returns
// the newest superblock. Throws Error(invalid_argument) for bad options.
Superblock make_filesystem(Store& store, const FormatOptions& options);

}  // namespace nanoer
