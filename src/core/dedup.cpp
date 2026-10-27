#include "nanoer/dedup.h"

#include "nanoer/error.h"

namespace nanoer {

// A target in one word: the record offset (8-byte aligned, below 2^48:
// 256 TiB), the packed flag in bit 0, the piece offset in the top 16 bits.
bool DedupIndex::fits(const DedupTarget& t) {
  return t.offset % 8 == 0 && !(t.offset >> 48) && !(t.piece >> 16) && (t.packed || !t.piece);
}

std::uint64_t DedupIndex::pack(const DedupTarget& t) {
  if (!fits(t)) throw Error(Errc::invalid_argument, "dedup target out of range");
  return t.offset | (t.packed ? 1 : 0) | (std::uint64_t{t.piece} << 48);
}

DedupTarget DedupIndex::unpack(std::uint64_t v) {
  return {v & 0x0000'ffff'ffff'fff8ull, (v & 1) != 0, static_cast<std::uint32_t>(v >> 48)};
}

std::size_t DedupIndex::home(std::uint64_t key) const {
  // The key is already a uniform hash; mix anyway in case of a weak one.
  key ^= key >> 31;
  key *= 0x9e3779b97f4a7c15ull;
  return static_cast<std::size_t>(key >> 7) & (slots_.size() - 1);
}

void DedupIndex::grow() {
  std::vector<Slot> old = std::move(slots_);
  slots_.assign(old.empty() ? 1024 : old.size() * 2, Slot{});
  size_ = 0;
  for (const Slot& s : old) {
    if (s.key) put(s.key, unpack(s.value));
  }
}

void DedupIndex::put(std::uint64_t key, const DedupTarget& target) {
  if (key == 0 || !fits(target)) return;
  std::uint64_t value = pack(target);
  if ((size_ + 1) * 4 > slots_.size() * 3) grow();
  for (std::size_t i = home(key);; i = (i + 1) & (slots_.size() - 1)) {
    if (slots_[i].key == key) {
      slots_[i].value = value;
      return;
    }
    if (slots_[i].key == 0) {
      slots_[i] = {key, value};
      ++size_;
      return;
    }
  }
}

std::optional<DedupTarget> DedupIndex::find(std::uint64_t key) const {
  if (key == 0 || slots_.empty()) return std::nullopt;
  for (std::size_t i = home(key);; i = (i + 1) & (slots_.size() - 1)) {
    if (slots_[i].key == key) return unpack(slots_[i].value);
    if (slots_[i].key == 0) return std::nullopt;
  }
}

void DedupIndex::erase(std::uint64_t key, const DedupTarget& target) {
  if (key == 0 || slots_.empty() || !fits(target)) return;
  const std::size_t mask = slots_.size() - 1;
  std::size_t i = home(key);
  while (slots_[i].key != key) {
    if (slots_[i].key == 0) return;
    i = (i + 1) & mask;
  }
  if (slots_[i].value != pack(target)) return;
  // Backward-shift: move later entries of the same run into the hole, so no
  // lookup stops early at it.
  for (std::size_t j = (i + 1) & mask;; j = (j + 1) & mask) {
    if (slots_[j].key == 0) break;
    std::size_t h = home(slots_[j].key);
    // Can the entry at j live at i? Only if its home is not in (i, j].
    bool in_range = i <= j ? (i < h && h <= j) : (i < h || h <= j);
    if (!in_range) {
      slots_[i] = slots_[j];
      i = j;
    }
  }
  slots_[i] = Slot{};
  --size_;
}

void DedupIndex::clear() {
  slots_.clear();
  size_ = 0;
}

}  // namespace nanoer
