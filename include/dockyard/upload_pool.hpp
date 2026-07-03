#pragma once

#include <dockyard/types.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <vector>

namespace dy::pool {

struct Stats {
  usize pending;
  u64 total_submitted;
  u64 total_completed;
};

// Generic future-lifecycle machinery shared by every async GPU-upload pool:
// submit CPU work + a completion callback, drain whatever has finished, and
// support cancellation via stop_source. Domain-specific commit (turning a
// ready Payload into a live Handle on the GPU) lives in each derived class,
// which needs the full SceneRenderer definition that this header avoids.
template <typename Payload, typename Handle> class UploadPool {
public:
  struct PendingWork {
    std::future<Payload> work;
    std::function<void(Handle)> on_complete;
    std::stop_source stop_src;
  };

  auto submit(std::future<Payload> work, std::stop_source stop_src,
              std::function<void(Handle)> on_complete) -> void {
    std::scoped_lock lock{work_mutex};
    if (stopped.load(std::memory_order_relaxed)) {
      stop_src.request_stop();
      return;
    }

    pending_work.push_back({
        .work = std::move(work),
        .on_complete = std::move(on_complete),
        .stop_src = std::move(stop_src),
    });
    submitted.fetch_add(1, std::memory_order_relaxed);
  }

  [[nodiscard]] auto take_ready(usize n) -> std::vector<PendingWork> {
    std::vector<PendingWork> batch;
    batch.reserve(n);

    std::scoped_lock lock{work_mutex};
    auto it = pending_work.begin();
    while (it != pending_work.end() && batch.size() < n) {
      if (it->work.wait_for(std::chrono::seconds{0}) ==
          std::future_status::ready) {
        batch.push_back(std::move(*it));
        it = pending_work.erase(it);
      } else {
        ++it;
      }
    }
    return batch;
  }

  [[nodiscard]] auto stats() const -> Stats {
    std::scoped_lock lock{work_mutex};
    return {
        .pending = pending_work.size(),
        .total_submitted = submitted.load(std::memory_order_relaxed),
        .total_completed = completed.load(std::memory_order_relaxed),
    };
  }

  [[nodiscard]] auto empty() const -> bool {
    std::scoped_lock lock{work_mutex};
    return pending_work.empty();
  }

  auto drop() -> void {
    std::scoped_lock lock{work_mutex};
    stopped.store(true, std::memory_order_relaxed);

    for (auto &entry : pending_work)
      entry.stop_src.request_stop();

    pending_work.clear();
  }

protected:
  auto note_completed(usize n) -> void {
    completed.fetch_add(n, std::memory_order_relaxed);
  }

private:
  mutable std::mutex work_mutex;
  std::vector<PendingWork> pending_work;
  std::atomic<bool> stopped{false};
  std::atomic<u64> submitted{0};
  std::atomic<u64> completed{0};
};

} // namespace dy::pool
