// How many references the kernel holds to each inode (FUSE lookups) or how
// many times each is open: an open-addressing table of 16-byte slots, so
// that a million inodes cached by the kernel cost about 24 MiB rather than
// the ≈ 40 MiB of a std::unordered_map (KNOWLEDGE.md §14).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "nanoer/ops.h"

namespace nanoer {

class LookupCounts {
 public:
  void add(InodeId ino, std::uint64_t n = 1) {
    if ((size_ + 1) * 4 > slots_.size() * 3) grow();
    Slot& s = slots_[find_slot(ino)];
    if (s.ino == 0) {
      s.ino = ino;
      ++size_;
    }
    s.count += n;
  }
  bool contains(InodeId ino) const { return !slots_.empty() && slots_[find_slot(ino)].ino == ino; }
  // Takes `n` away (down to 0, when the inode leaves the table); returns
  // what is left, or nothing if the inode was not there.
  std::optional<std::uint64_t> drop(InodeId ino, std::uint64_t n) {
    if (slots_.empty()) return std::nullopt;
    std::size_t i = find_slot(ino);
    if (slots_[i].ino != ino) return std::nullopt;
    std::uint64_t& c = slots_[i].count;
    c = c > n ? c - n : 0;
    if (c != 0) return c;
    erase_at(i);
    return 0;
  }
  void clear() {
    slots_.clear();
    size_ = 0;
  }
  std::size_t size() const { return size_; }

 private:
  struct Slot {
    InodeId ino = 0;  // 0: empty (no inode has number 0)
    std::uint64_t count = 0;
  };

  std::size_t home(InodeId ino) const {
    return static_cast<std::size_t>((ino * 0x9E3779B97F4A7C15ull) >> 32) & (slots_.size() - 1);
  }
  // The inode's slot, or the empty one where it would go (linear probing).
  std::size_t find_slot(InodeId ino) const {
    std::size_t i = home(ino);
    while (slots_[i].ino != 0 && slots_[i].ino != ino) i = (i + 1) & (slots_.size() - 1);
    return i;
  }
  // Backward-shift deletion: no tombstones, so lookups stay short.
  void erase_at(std::size_t i) {
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t j = (i + 1) & mask; slots_[j].ino != 0; j = (j + 1) & mask) {
      std::size_t h = home(slots_[j].ino);
      // Move j into the hole at i if its home is not in (i, j] (cyclically).
      if (((j - h) & mask) >= ((j - i) & mask)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i] = {};
    --size_;
  }
  void grow() {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(old.empty() ? 64 : old.size() * 2, {});
    size_ = 0;
    for (const Slot& s : old) {
      if (s.ino != 0) add(s.ino, s.count);
    }
  }

  std::vector<Slot> slots_;
  std::size_t size_ = 0;
};

}  // namespace nanoer
