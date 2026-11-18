#include "gpu_progress.h"

namespace nier::native {

namespace {

GpuProgress g_progress;
std::atomic<bool> g_enabled{false};

}  // namespace

void GpuProgress::Advance() {
  value_.fetch_add(1, std::memory_order_seq_cst);
  if (waiters_.load(std::memory_order_seq_cst)) {
    { std::lock_guard<std::mutex> lock(mutex_); }
    cv_.notify_all();
    notifies_.fetch_add(1, std::memory_order_relaxed);
  }
}

uint64_t GpuProgress::WaitPast(uint64_t seen, std::chrono::microseconds timeout) {
  uint64_t value = value_.load(std::memory_order_seq_cst);
  if (value != seen) {
    return value;
  }
  waiters_.fetch_add(1, std::memory_order_seq_cst);
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return value_.load(std::memory_order_seq_cst) != seen; });
  }
  waiters_.fetch_sub(1, std::memory_order_seq_cst);
  return value_.load(std::memory_order_seq_cst);
}

GpuProgress& SharedGpuProgress() {
  return g_progress;
}

void EnableGpuProgress(bool enabled) {
  g_enabled.store(enabled, std::memory_order_release);
}

GpuProgress* ActiveGpuProgress() {
  return g_enabled.load(std::memory_order_acquire) ? &g_progress : nullptr;
}

}  // namespace nier::native
