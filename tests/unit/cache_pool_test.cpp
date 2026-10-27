#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <stdexcept>

#include <gtest/gtest.h>

#include "nanoer/cache.h"
#include "nanoer/thread_pool.h"

using namespace nanoer;

TEST(ChunkCache, TwoQStaysWithinBudget) {
  ChunkCache cache(1000, 1);
  for (std::uint64_t i = 0; i < 10; ++i) {
    cache.put({i, 1}, std::make_shared<const Bytes>(300, static_cast<std::uint8_t>(i)));
    cache.get({i, 1});  // used twice: into the LRU
    cache.get({0, 1});
    EXPECT_LE(cache.resident_bytes(), 1000u);
  }
  ASSERT_NE(cache.get({0, 1}), nullptr) << "the most used";
  ASSERT_NE(cache.get({9, 1}), nullptr) << "the newest";
}

TEST(ChunkCache, StaysWithinBudgetAndEvictsOldest) {
  ChunkCache cache(1000, 1, false);
  for (std::uint64_t i = 0; i < 10; ++i) {
    cache.put({i, 1}, std::make_shared<const Bytes>(300, static_cast<std::uint8_t>(i)));
    EXPECT_LE(cache.resident_bytes(), 1000u);
  }
  EXPECT_EQ(cache.get({0, 1}), nullptr);
  ASSERT_NE(cache.get({9, 1}), nullptr);
  EXPECT_EQ((*cache.get({9, 1}))[0], 9);
}

TEST(ChunkCache, RecentUseProtectsEntries) {
  ChunkCache cache(900, 1, false);
  cache.put({1, 1}, std::make_shared<const Bytes>(300));
  cache.put({2, 1}, std::make_shared<const Bytes>(300));
  cache.put({3, 1}, std::make_shared<const Bytes>(300));
  cache.get({1, 1});  // now the most recently used
  cache.put({4, 1}, std::make_shared<const Bytes>(300));
  EXPECT_NE(cache.get({1, 1}), nullptr);
  EXPECT_EQ(cache.get({2, 1}), nullptr);
}

TEST(ChunkCache, SequenceNumberSeparatesReusedLocations) {
  ChunkCache cache(1 << 20);
  cache.put({4096, 1}, std::make_shared<const Bytes>(10, 1));
  EXPECT_EQ(cache.get({4096, 2}), nullptr);  // same place, segment reused since
  cache.put({4096, 2}, std::make_shared<const Bytes>(10, 2));
  EXPECT_EQ((*cache.get({4096, 1}))[0], 1);
  EXPECT_EQ((*cache.get({4096, 2}))[0], 2);
}

TEST(ChunkCache, OversizedChunksAreNotKept) {
  ChunkCache cache(1000, 4);  // 250 bytes per shard
  cache.put({1, 1}, std::make_shared<const Bytes>(251));
  EXPECT_EQ(cache.get({1, 1}), nullptr);
  EXPECT_EQ(cache.resident_bytes(), 0u);
}

TEST(ChunkCache, RecordsOfOneSegmentUseEveryShard) {
  // Record offsets are multiples of 8, and one segment's share a sequence
  // number: they must still spread over all the shards, or the cache holds
  // only a fraction of its budget.
  ChunkCache cache(16 * 1000, 16);
  for (std::uint64_t i = 0; i < 100; ++i) cache.put({(1 << 20) + i * 65544, 3}, std::make_shared<const Bytes>(100));
  EXPECT_GE(cache.resident_bytes(), 9000u);
}

TEST(ChunkCache, TwoQKeepsWhatIsUsedAgainThroughAScan) {
  for (bool two_q : {false, true}) {
    ChunkCache cache(64 * 100, 1, two_q);
    auto chunk = std::make_shared<const Bytes>(100);
    auto touch = [&](std::uint64_t k) {
      if (cache.get({k, 1})) return true;
      cache.put({k, 1}, chunk);
      return false;
    };
    for (int round = 0; round < 3; ++round) {
      for (std::uint64_t k = 0; k < 32; ++k) touch(k);  // a working set of half the budget
    }
    int hits = 0;
    for (std::uint64_t s = 0; s < 1000; ++s) {
      touch(1000 + s);  // a scan of 16 times the budget
      if (s % 10 == 0) hits += touch(s / 10 % 32);
    }
    if (two_q) {
      EXPECT_EQ(hits, 100) << "2Q: the working set survives the scan";
    } else {
      EXPECT_LT(hits, 10) << "LRU: the scan evicts it";
    }
  }
}

TEST(ChunkCache, ConcurrentMissesLoadOnce) {
  ChunkCache cache(1 << 20);
  std::atomic<int> loads{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      auto data = cache.get_or_load({4096, 1}, [&] {
        ++loads;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // a slow decompression
        return std::make_shared<const Bytes>(100, 7);
      });
      EXPECT_EQ((*data)[0], 7);
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(loads, 1);
  // A failed load reaches the caller and is not kept.
  EXPECT_THROW(cache.get_or_load({8192, 1}, []() -> std::shared_ptr<const Bytes> { throw std::runtime_error("x"); }),
               std::runtime_error);
  EXPECT_EQ(cache.get({8192, 1}), nullptr);
}

TEST(ThreadPool, RunsEverythingExactlyOnce) {
  for (unsigned threads : {0u, 1u, 4u}) {
    ThreadPool pool(threads);
    for (int round = 0; round < 50; ++round) {
      std::vector<std::atomic<int>> hits(97);
      pool.parallel_for(hits.size(), [&](std::size_t i) { ++hits[i]; });
      for (auto& h : hits) ASSERT_EQ(h.load(), 1);
    }
  }
}

TEST(ThreadPool, PropagatesExceptions) {
  ThreadPool pool(3);
  EXPECT_THROW(pool.parallel_for(20,
                                 [](std::size_t i) {
                                   if (i == 13) throw std::runtime_error("boom");
                                 }),
               std::runtime_error);
  std::atomic<int> n{0};
  pool.parallel_for(5, [&](std::size_t) { ++n; });  // still usable
  EXPECT_EQ(n, 5);
}
