#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace nier::native {

class GpuProgress {
 public:
  void Advance();
  uint64_t WaitPast(uint64_t seen, std::chrono::microseconds timeout);
  uint64_t notifies() const { return notifies_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> value_{0};
  std::atomic<uint64_t> notifies_{0};
  std::atomic<uint32_t> waiters_{0};
  std::mutex mutex_;
  std::condition_variable cv_;
};

GpuProgress& SharedGpuProgress();
void EnableGpuProgress(bool enabled);
GpuProgress* ActiveGpuProgress();

}  // namespace nier::native
