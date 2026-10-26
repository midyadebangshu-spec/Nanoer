// Store geometry and the superblock. The byte-level specification is
// docs/format.md; keep the two in sync.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "nanoer/bytes.h"
#include "nanoer/codec.h"

namespace nanoer {

class Store;

inline constexpr std::uint32_t kBlockSize = 4096;
inline constexpr std::array<std::uint64_t, 2> kSuperblockOffsets = {0, kBlockSize};
inline constexpr std::uint64_t kDataStart = 1u << 20;  // segment 0 starts at 1 MiB

inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;

inline constexpr unsigned kMinSegmentShift = 20;  // 1 MiB
inline constexpr unsigned kMaxSegmentShift = 30;  // 1 GiB
inline constexpr unsigned kDefaultSegmentShift = 26;  // 64 MiB
inline constexpr unsigned kMinChunkShift = 12;  // 4 KiB
inline constexpr unsigned kMaxChunkShift = 24;  // 16 MiB
inline constexpr unsigned kDefaultChunkShift = 17;  // 128 KiB
inline constexpr std::uint64_t kMinSegmentCount = 4;
inline constexpr std::size_t kMaxLabelLength = 63;

// Feature flags.
//   compat:     safe to ignore
//   ro_compat:  readable without understanding, but not writable
//   incompat:   must be understood to read the filesystem at all
namespace incompat_features {
// A data chunk record may have several references (dedup) or hold several
// small files (a pack): SetChunkRef operations, version 2 checkpoints.
inline constexpr std::uint64_t shared_records = 0x1;
}  // namespace incompat_features
namespace compat_features {
// A preference: do not deduplicate new data (mkfs.nanoer --no-dedup).
inline constexpr std::uint64_t no_dedup = 0x1;
}  // namespace compat_features
inline constexpr std::uint64_t kKnownCompatFeatures = compat_features::no_dedup;
inline constexpr std::uint64_t kKnownRoCompatFeatures = 0;
inline constexpr std::uint64_t kKnownIncompatFeatures = incompat_features::shared_records;

// Superblock state flags (not features: any version may ignore them).
namespace state_flags {
inline constexpr std::uint32_t errors = 0x01;  // damage was seen; run nanoer fsck
}

using Uuid = std::array<std::uint8_t, 16>;

Uuid random_uuid();
std::string uuid_to_string(const Uuid& uuid);
std::uint64_t now_ns();

struct Superblock {
  std::uint16_t version_major = kVersionMajor;
  std::uint16_t version_minor = kVersionMinor;
  std::uint32_t block_size = kBlockSize;
  Uuid uuid{};
  std::uint64_t generation = 0;
  std::uint64_t features_compat = 0;
  std::uint64_t features_ro_compat = 0;
  std::uint64_t features_incompat = 0;
  std::uint64_t store_size = 0;
  std::uint64_t data_start = kDataStart;
  std::uint32_t segment_shift = kDefaultSegmentShift;
  std::uint64_t segment_count = 0;
  std::uint64_t created_ns = 0;
  std::uint64_t updated_ns = 0;
  std::uint64_t next_segment_seq = 1;
  std::uint64_t checkpoint_offset = 0;  // 0: no checkpoint yet (empty filesystem)
  std::uint64_t checkpoint_seq = 0;
  CodecId default_codec = CodecId::zstd;
  std::int8_t default_level = 1;
  std::uint8_t chunk_shift = kDefaultChunkShift;
  std::uint32_t state = 0;  // state_flags
  std::string label;

  std::uint64_t segment_size() const { return std::uint64_t{1} << segment_shift; }
  std::uint32_t chunk_size() const { return std::uint32_t{1} << chunk_shift; }
  std::uint64_t segment_offset(std::uint64_t index) const {
    return data_start + index * segment_size();
  }
};

inline bool has_shared_records(const Superblock& sb) {
  return (sb.features_incompat & incompat_features::shared_records) != 0;
}

Bytes encode_superblock(const Superblock& sb);

struct DecodedSuperblock {
  enum class State {
    invalid,      // not a superblock, or damaged (bad magic, checksum or fields)
    unsupported,  // intact, but needs a newer Nanoer (version or incompat feature)
    valid,
  };
  State state = State::invalid;
  Superblock sb;
  std::string reason;  // why it is not valid
};

DecodedSuperblock decode_superblock(ByteView block);

struct LoadedSuperblock {
  Superblock sb;
  int slot;        // 0 or 1
  bool read_only;  // has ro_compat features this build does not know
  // The other slot, if it is valid and older: the previous checkpoint, to
  // fall back on if the newest one is damaged.
  std::optional<Superblock> older;
};

// Reads both superblock slots and returns the newest valid one. Throws
// Error(corrupt) if neither is valid, or Error(unsupported) if the newest
// intact one needs a newer Nanoer: falling back to an older slot then would
// silently lose newer data.
LoadedSuperblock load_superblock(Store& store);

// Increments sb.generation, updates sb.updated_ns, writes the superblock to
// slot (generation % 2) and flushes. The previous superblock in the other
// slot stays intact, so a torn write here can never lose the filesystem.
void write_superblock(Store& store, Superblock& sb);

}  // namespace nanoer
