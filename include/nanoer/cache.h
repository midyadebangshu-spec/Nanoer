// The safe space: a bounded cache of decompressed chunks (KNOWLEDGE.md §8).
//
// Split into shards, each with its own lock and LRU list, so many threads
// reading different chunks rarely wait on each other. A chunk is identified
// by its record's location plus its segment's sequence number: when a freed
// segment is reused, the same offset holds different data under a new
// sequence number, so stale entries can never be returned.
#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nanoer/bytes.h"

namespace nanoer {

class ChunkCache {
 public:
  struct Key {
    std::uint64_t offset;
    std::uint64_t seq;
    bool operator==(const Key&) const = default;
  };

  explicit ChunkCache(std::uint64_t budget_bytes, unsigned shards = 16);

  std::shared_ptr<const Bytes> get(const Key& key);
  // Chunks larger than a shard's share of the budget are not kept.
  void put(const Key& key, std::shared_ptr<const Bytes> data);

  std::uint64_t budget() const { return budget_; }
  std::uint64_t resident_bytes() const;
  std::uint64_t hits() const { return hits_; }
  std::uint64_t misses() const { return misses_; }

 private:
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
      return std::hash<std::uint64_t>()(k.offset * 0x9e3779b97f4a7c15ull ^ k.seq);
    }
  };
  struct Shard {
    std::mutex mutex;
    std::list<std::pair<Key, std::shared_ptr<const Bytes>>> lru;  // most recent first
    std::unordered_map<Key, decltype(lru)::iterator, KeyHash> index;
    std::uint64_t bytes = 0;
  };
  Shard& shard_for(const Key& key);

  std::uint64_t budget_;
  std::uint64_t shard_budget_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> misses_{0};
};

}  // namespace nanoer
