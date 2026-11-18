#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "gpu_progress.h"

using nier::native::ActiveGpuProgress;
using nier::native::EnableGpuProgress;
using nier::native::GpuProgress;
using nier::native::SharedGpuProgress;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  {
    GpuProgress progress;
    progress.Advance();
    const auto start = Clock::now();
    const uint64_t seen = progress.WaitPast(0, 1s);
    Check(seen == 1 && Clock::now() - start < 100ms, "wait_returns_at_once_when_progress_moved");
  }
  {
    GpuProgress progress;
    const auto start = Clock::now();
    const uint64_t seen = progress.WaitPast(0, 20ms);
    Check(seen == 0 && Clock::now() - start >= 20ms, "wait_times_out_without_progress");
  }
  {
    GpuProgress progress;
    std::thread advancer([&] {
      std::this_thread::sleep_for(30ms);
      progress.Advance();
    });
    const auto start = Clock::now();
    const uint64_t seen = progress.WaitPast(0, 5s);
    advancer.join();
    Check(seen == 1 && Clock::now() - start < 1s, "advance_from_another_thread_wakes_the_waiter");
  }
  {
    GpuProgress progress;
    progress.Advance();
    const uint64_t quiet = progress.notifies();
    std::thread advancer([&] {
      std::this_thread::sleep_for(30ms);
      progress.Advance();
    });
    progress.WaitPast(1, 5s);
    advancer.join();
    Check(quiet == 0 && progress.notifies() == 1, "notifies_counted_only_with_waiters");
  }
  {
    Check(ActiveGpuProgress() == nullptr, "inactive_until_enabled");
    EnableGpuProgress(true);
    Check(ActiveGpuProgress() == &SharedGpuProgress(), "active_after_enable");
    EnableGpuProgress(false);
    Check(ActiveGpuProgress() == nullptr, "inactive_after_disable");
  }
  return failures ? 1 : 0;
}
