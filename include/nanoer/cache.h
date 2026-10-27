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
#include <functional>
#include <future>
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

  // Scan-resistant (simplified 2Q, Johnson and Shasha 1994): chunks seen
  // once wait in a FIFO of up to a quarter of the budget, and only chunks
  // asked for again enter the LRU, so one large scan (a backup, a search)
  // cannot push out what is used repeatedly (measured: KNOWLEDGE.md §14).
  // `two_q` false: a plain LRU.
  explicit ChunkCache(std::uint64_t budget_bytes, unsigned shards = 16, bool two_q = true);

  std::shared_ptr<const Bytes> get(const Key& key);
  // The chunk, from the cache or from `load` (then kept). Threads asking for
  // the same missing chunk at once wait for one load instead of each
  // decompressing it (the kernel reads ahead in parallel, often within one
  // cold unit). An exception from `load` reaches every waiter.
  std::shared_ptr<const Bytes> get_or_load(const Key& key,
                                           const std::function<std::shared_ptr<const Bytes>()>& load);
  // Chunks larger than a shard's share of the budget are not kept.
  void put(const Key& key, std::shared_ptr<const Bytes> data);

  std::uint64_t budget() const { return budget_; }
  std::uint64_t resident_bytes() const;
  std::uint64_t hits() const { return hits_; }
  std::uint64_t misses() const { return misses_; }

 private:
  struct KeyHash {
    // Offsets are multiples of 8 and a segment's records share its seq:
    // every bit must be mixed into the low ones, which pick the shard.
    std::size_t operator()(const Key& k) const noexcept {
      std::uint64_t x = k.offset ^ (k.seq * 0x9e3779b97f4a7c15ull);
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdull;
      x ^= x >> 33;
      x *= 0xc4ceb9fe1a85ec53ull;
      x ^= x >> 33;
      return static_cast<std::size_t>(x);
    }
  };
  struct Shard {
    std::mutex mutex;
    std::list<std::pair<Key, std::shared_ptr<const Bytes>>> lru;  // most recent first
    std::unordered_map<Key, decltype(lru)::iterator, KeyHash> index;
    std::uint64_t bytes = 0;
    // 2Q: chunks seen once (FIFO).
    std::list<std::pair<Key, std::shared_ptr<const Bytes>>> in;
    std::unordered_map<Key, decltype(in)::iterator, KeyHash> in_index;
    std::uint64_t in_bytes = 0;
    std::unordered_map<Key, std::shared_future<std::shared_ptr<const Bytes>>, KeyHash> loading;
  };
  std::shared_ptr<const Bytes> find(Shard& s, const Key& key);  // s.mutex held; counts the hit
  void insert(Shard& s, const Key& key, std::shared_ptr<const Bytes> data);  // s.mutex held
  void evict(Shard& s);
  Shard& shard_for(const Key& key);

  std::uint64_t budget_;
  std::uint64_t shard_budget_;
  bool two_q_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> misses_{0};
};

}  // namespace nanoer
