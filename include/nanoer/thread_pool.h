// A small fixed-size thread pool for parallel compression.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nanoer {

class ThreadPool {
 public:
  // 0 threads means "run everything on the calling thread".
  explicit ThreadPool(unsigned threads);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Calls fn(0) .. fn(n - 1), spread over the pool and the calling thread,
  // and returns when all are done. Rethrows the first exception thrown.
  void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn);

  unsigned size() const { return static_cast<unsigned>(workers_.size()); }

 private:
  struct Batch;
  void worker();
  static void run(Batch& batch);

  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Batch*> queue_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace nanoer
