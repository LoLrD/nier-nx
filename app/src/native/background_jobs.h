#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace nier::native {

template <typename Result>
class BackgroundJobs {
 public:
  explicit BackgroundJobs(std::function<void()> on_start)
      : worker_([this, on_start = std::move(on_start)] {
          on_start();
          Work();
        }) {}

  ~BackgroundJobs() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    worker_.join();
  }

  BackgroundJobs(const BackgroundJobs&) = delete;
  BackgroundJobs& operator=(const BackgroundJobs&) = delete;

  void Submit(uint64_t key, std::function<Result()> work) {
    {
      std::lock_guard lock(mutex_);
      states_[key] = std::nullopt;
      queue_.emplace_back(key, std::move(work));
    }
    ready_.notify_one();
  }

  bool Pending(uint64_t key) const {
    std::lock_guard lock(mutex_);
    return states_.contains(key);
  }

  bool Done(uint64_t key) const {
    std::lock_guard lock(mutex_);
    const auto it = states_.find(key);
    return it != states_.end() && it->second.has_value();
  }

  std::optional<Result> Take(uint64_t key) {
    std::lock_guard lock(mutex_);
    const auto it = states_.find(key);
    if (it == states_.end() || !it->second) {
      return std::nullopt;
    }
    std::optional<Result> result = std::move(it->second);
    states_.erase(it);
    return result;
  }

 private:
  void Work() {
    std::unique_lock lock(mutex_);
    while (true) {
      ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) {
        return;
      }
      auto [key, work] = std::move(queue_.front());
      queue_.pop_front();
      lock.unlock();
      Result result = work();
      lock.lock();
      if (const auto it = states_.find(key); it != states_.end()) {
        it->second = std::move(result);
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::pair<uint64_t, std::function<Result()>>> queue_;
  std::unordered_map<uint64_t, std::optional<Result>> states_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace nier::native
