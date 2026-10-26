// Where the space goes (nanoer stats; docs/M4_PLAN.md §6). Computed from the
// metadata (plus one header read per pack, for its size), so it is cheap
// enough for a mounted filesystem, which serves it as the read-only extended
// attribute user.nanoer.stats on its root directory.
//
// The logical size breaks down as: stored = logical - holes and zeros -
// dedup - compression + dead space in packs.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

namespace nanoer {

// The prefix reserved for Nanoer's own extended attributes.
inline constexpr const char* kNanoerXattrPrefix = "user.nanoer.";
inline constexpr const char* kStatsXattr = "user.nanoer.stats";

struct SpaceStats {
  // Contents.
  std::uint64_t files = 0;
  std::uint64_t directories = 0;
  std::uint64_t other_inodes = 0;
  std::uint64_t logical_bytes = 0;     // the files' sizes
  std::uint64_t referenced_bytes = 0;  // data in chunks, once per reference: holes and zeros excluded
  std::uint64_t dedup_bytes = 0;       // references beyond the first to the same data
  std::uint64_t unique_bytes = 0;      // uncompressed data stored, each record once (packs whole)
  std::uint64_t pack_dead_bytes = 0;   // in packs, no longer referenced (GC repacks it)
  std::uint64_t stored_bytes = 0;      // what the records take on disk
  std::uint64_t records = 0;
  std::uint64_t shared_records = 0;  // with data referred to more than once (dedup)
  std::uint64_t packs = 0;
  std::uint64_t packed_files = 0;  // chunks that are pieces of a pack

  // Space.
  std::uint64_t segment_bytes = 0;  // data a segment holds
  std::uint64_t segments = 0;
  std::uint64_t segments_in_use = 0;
  std::uint64_t segments_pending = 0;  // free after the next checkpoint
  std::uint64_t segments_free = 0;
  std::uint64_t live_bytes = 0;  // in segments in use, by the space accounting

  // Garbage collection, since mounting.
  std::uint64_t gc_bytes_copied = 0;
  std::uint64_t gc_records_copied = 0;
  std::uint64_t gc_segments_emptied = 0;
  std::uint64_t gc_packs_repacked = 0;
  std::uint64_t gc_damaged = 0;

  // Dedup and memory.
  bool dedup = false;
  std::uint64_t dedup_entries = 0;
  std::uint64_t dedup_index_bytes = 0;
  std::uint64_t metadata_bytes = 0;  // an estimate of the metadata held in memory

  // Stored data by codec (only when asked for: every record header is read).
  struct Codec {
    std::uint64_t records = 0;
    std::uint64_t data_bytes = 0;
    std::uint64_t stored_bytes = 0;
  };
  std::map<std::string, Codec> by_codec;

  // What each technique saved, in bytes of logical size.
  std::uint64_t saved_by_holes() const { return logical_bytes - std::min(logical_bytes, referenced_bytes); }
  std::uint64_t saved_by_compression() const {
    return unique_bytes > stored_bytes ? unique_bytes - stored_bytes : 0;
  }
};

// "key=value" lines, for the extended attribute.
std::string encode_stats(const SpaceStats& stats);
SpaceStats decode_stats(const std::string& text);

}  // namespace nanoer
