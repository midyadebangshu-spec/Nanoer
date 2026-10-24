#include "nanoer/cache.h"

namespace nanoer {

ChunkCache::ChunkCache(std::uint64_t budget_bytes, unsigned shards)
    : budget_(budget_bytes), shard_budget_(budget_bytes / (shards ? shards : 1)) {
  for (unsigned i = 0; i < (shards ? shards : 1); ++i) shards_.push_back(std::make_unique<Shard>());
}

ChunkCache::Shard& ChunkCache::shard_for(const Key& key) {
  return *shards_[KeyHash()(key) % shards_.size()];
}

std::shared_ptr<const Bytes> ChunkCache::get(const Key& key) {
  Shard& s = shard_for(key);
  std::lock_guard lock(s.mutex);
  auto it = s.index.find(key);
  if (it == s.index.end()) {
    ++misses_;
    return nullptr;
  }
  s.lru.splice(s.lru.begin(), s.lru, it->second);
  ++hits_;
  return it->second->second;
}

void ChunkCache::put(const Key& key, std::shared_ptr<const Bytes> data) {
  if (!data || data->size() > shard_budget_) return;
  Shard& s = shard_for(key);
  std::lock_guard lock(s.mutex);
  if (auto it = s.index.find(key); it != s.index.end()) {
    s.bytes -= it->second->second->size();
    s.lru.erase(it->second);
    s.index.erase(it);
  }
  s.bytes += data->size();
  s.lru.emplace_front(key, std::move(data));
  s.index.emplace(key, s.lru.begin());
  while (s.bytes > shard_budget_) {
    auto& [old_key, old_data] = s.lru.back();
    s.bytes -= old_data->size();
    s.index.erase(old_key);
    s.lru.pop_back();
  }
}

std::uint64_t ChunkCache::resident_bytes() const {
  std::uint64_t total = 0;
  for (const auto& s : shards_) {
    std::lock_guard lock(s->mutex);
    total += s->bytes;
  }
  return total;
}

}  // namespace nanoer
