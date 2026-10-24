#include "nanoer/thread_pool.h"

#include <atomic>
#include <exception>

namespace nanoer {

struct ThreadPool::Batch {
  std::size_t n;
  const std::function<void(std::size_t)>* fn;
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> finished{0};
  int workers = 0;  // pool threads currently inside run(); guarded by `mutex`
  std::mutex mutex;
  std::condition_variable done;
  std::exception_ptr error;
};

ThreadPool::ThreadPool(unsigned threads) {
  for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { worker(); });
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  for (auto& t : workers_) t.join();
}

void ThreadPool::run(Batch& batch) {
  while (true) {
    std::size_t i = batch.next++;
    if (i >= batch.n) return;
    try {
      (*batch.fn)(i);
    } catch (...) {
      std::lock_guard lock(batch.mutex);
      if (!batch.error) batch.error = std::current_exception();
    }
    if (++batch.finished == batch.n) {
      std::lock_guard lock(batch.mutex);
      batch.done.notify_all();
    }
  }
}

void ThreadPool::worker() {
  while (true) {
    Batch* batch;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stopping
      batch = queue_.back();
      // Leave the batch queued while it still has unclaimed items, so other
      // workers can join in; whoever finds it exhausted removes it.
      if (batch->next >= batch->n) {
        queue_.pop_back();
        continue;
      }
      // Registered while mutex_ is held, so parallel_for() cannot remove the
      // batch and return without waiting for this thread.
      std::lock_guard batch_lock(batch->mutex);
      ++batch->workers;
    }
    run(*batch);
    std::lock_guard batch_lock(batch->mutex);
    --batch->workers;
    batch->done.notify_all();
  }
}

void ThreadPool::parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
  if (n == 0) return;
  Batch batch;
  batch.n = n;
  batch.fn = &fn;
  if (!workers_.empty() && n > 1) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(&batch);
    }
    wake_.notify_all();
  }
  run(batch);
  {
    std::unique_lock lock(batch.mutex);
    batch.done.wait(lock, [&] { return batch.finished == batch.n; });
  }
  {
    // The batch lives on this stack frame: no worker may pick it up any more...
    std::lock_guard lock(mutex_);
    std::erase(queue_, &batch);
  }
  {
    // ...and every worker that did must have left it.
    std::unique_lock lock(batch.mutex);
    batch.done.wait(lock, [&] { return batch.workers == 0; });
  }
  if (batch.error) std::rethrow_exception(batch.error);
}

}  // namespace nanoer
