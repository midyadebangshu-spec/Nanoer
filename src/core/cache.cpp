#include "nanoer/cache.h"

namespace nanoer {

ChunkCache::ChunkCache(std::uint64_t budget_bytes, unsigned shards, bool two_q)
    : budget_(budget_bytes), shard_budget_(budget_bytes / (shards ? shards : 1)), two_q_(two_q) {
  for (unsigned i = 0; i < (shards ? shards : 1); ++i) shards_.push_back(std::make_unique<Shard>());
}

ChunkCache::Shard& ChunkCache::shard_for(const Key& key) {
  return *shards_[KeyHash()(key) % shards_.size()];
}

std::shared_ptr<const Bytes> ChunkCache::find(Shard& s, const Key& key) {
  auto it = s.index.find(key);
  if (it == s.index.end()) {
    auto i = s.in_index.find(key);
    if (i == s.in_index.end()) return nullptr;
    // 2Q: asked for again, so it is not a scan's: into the LRU.
    ++hits_;
    s.in_bytes -= i->second->second->size();
    s.lru.splice(s.lru.begin(), s.in, i->second);
    s.index.emplace(key, s.lru.begin());
    s.in_index.erase(i);
    return s.lru.front().second;
  }
  s.lru.splice(s.lru.begin(), s.lru, it->second);
  ++hits_;
  return it->second->second;
}

std::shared_ptr<const Bytes> ChunkCache::get(const Key& key) {
  Shard& s = shard_for(key);
  std::lock_guard lock(s.mutex);
  std::shared_ptr<const Bytes> data = find(s, key);
  if (!data) ++misses_;
  return data;
}

std::shared_ptr<const Bytes> ChunkCache::get_or_load(const Key& key,
                                                     const std::function<std::shared_ptr<const Bytes>()>& load) {
  Shard& s = shard_for(key);
  std::promise<std::shared_ptr<const Bytes>> promise;
  {
    std::unique_lock lock(s.mutex);
    if (std::shared_ptr<const Bytes> data = find(s, key)) return data;
    if (auto it = s.loading.find(key); it != s.loading.end()) {
      auto pending = it->second;
      lock.unlock();
      ++hits_;  // decompressed once, for several readers
      return pending.get();
    }
    ++misses_;
    s.loading.emplace(key, promise.get_future().share());
  }
  std::shared_ptr<const Bytes> data;
  try {
    data = load();
  } catch (...) {
    std::lock_guard lock(s.mutex);
    s.loading.erase(key);
    promise.set_exception(std::current_exception());
    throw;
  }
  std::lock_guard lock(s.mutex);
  s.loading.erase(key);
  if (data && data->size() <= shard_budget_) insert(s, key, data);
  promise.set_value(data);
  return data;
}

void ChunkCache::put(const Key& key, std::shared_ptr<const Bytes> data) {
  if (!data || data->size() > shard_budget_) return;
  Shard& s = shard_for(key);
  std::lock_guard lock(s.mutex);
  insert(s, key, std::move(data));
}

void ChunkCache::insert(Shard& s, const Key& key, std::shared_ptr<const Bytes> data) {
  if (auto it = s.index.find(key); it != s.index.end()) {
    s.bytes -= it->second->second->size();
    s.lru.erase(it->second);
    s.index.erase(it);
  }
  if (auto it = s.in_index.find(key); it != s.in_index.end()) {
    s.bytes -= it->second->second->size();
    s.in_bytes -= it->second->second->size();
    s.in.erase(it->second);
    s.in_index.erase(it);
  }
  s.bytes += data->size();
  if (two_q_) {
    s.in_bytes += data->size();
    s.in.emplace_front(key, std::move(data));
    s.in_index.emplace(key, s.in.begin());
  } else {
    s.lru.emplace_front(key, std::move(data));
    s.index.emplace(key, s.lru.begin());
  }
  evict(s);
}

void ChunkCache::evict(Shard& s) {
  while (s.bytes > shard_budget_) {
    // 2Q: the FIFO of chunks seen once gives way first, down to a quarter
    // of the budget (but the newest stays, however large).
    if (two_q_ && !s.in.empty() && ((s.in_bytes > shard_budget_ / 4 && s.in.size() > 1) || s.lru.empty())) {
      auto& [old_key, old_data] = s.in.back();
      s.bytes -= old_data->size();
      s.in_bytes -= old_data->size();
      s.in_index.erase(old_key);
      s.in.pop_back();
      continue;
    }
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
