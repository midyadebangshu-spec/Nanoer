// The dedup index: dedup key (the first 8 bytes of a chunk's BLAKE3) -> where
// a record with that content is (docs/M4_PLAN.md §4). A hint only: a hit is
// confirmed against the full BLAKE3 in the record before it is used, so a
// colliding key just means a missed duplicate.
//
// Open addressing with linear probing and backward-shift deletion: 16 bytes
// an entry, about 21 at the load it keeps (KNOWLEDGE.md §6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nanoer {

struct DedupTarget {
  std::uint64_t offset = 0;  // the record (8-byte aligned)
  bool packed = false;       // a piece of a pack ...
  std::uint32_t piece = 0;   // ... starting here in it (packs are at most 64 KiB)
  bool operator==(const DedupTarget&) const = default;
};

class DedupIndex {
 public:
  // Points `key` at `target`, replacing what it pointed at (the newest
  // record with a content wins: after GC moves a record, its copy).
  void put(std::uint64_t key, const DedupTarget& target);
  std::optional<DedupTarget> find(std::uint64_t key) const;
  // Removes `key`, if it still points at `target`.
  void erase(std::uint64_t key, const DedupTarget& target);

  std::size_t size() const { return size_; }
  std::size_t memory_bytes() const { return slots_.size() * sizeof(Slot); }
  void clear();

  // Derived from the references, and the same content may be indexed at
  // either of two places: not part of the state that must match.
  bool operator==(const DedupIndex&) const { return true; }

 private:
  struct Slot {
    std::uint64_t key = 0;  // 0: empty (no dedup key is 0)
    std::uint64_t value = 0;
  };
  static std::uint64_t pack(const DedupTarget& t);
  static DedupTarget unpack(std::uint64_t v);
  std::size_t home(std::uint64_t key) const;
  void grow();

  std::vector<Slot> slots_;  // a power of two in size, or empty
  std::size_t size_ = 0;
};

}  // namespace nanoer
