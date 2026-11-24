#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "background_jobs.h"

using nier::native::BackgroundJobs;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

template <typename Predicate>
static bool WaitFor(Predicate done) {
  for (int i = 0; i < 2000; ++i) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

int main() {
  {
    std::atomic<bool> release{false};
    BackgroundJobs<int> jobs([] {});
    Check(!jobs.Pending(1) && !jobs.Take(1), "unknown_key_is_not_pending");
    jobs.Submit(1, [&] {
      while (!release.load()) {
        std::this_thread::yield();
      }
      return 10;
    });
    jobs.Submit(2, [] { return 20; });
    Check(jobs.Pending(1) && jobs.Pending(2), "submitted_keys_are_pending");
    Check(!jobs.Take(1), "unfinished_job_has_no_result");
    release = true;
    Check(WaitFor([&] { return jobs.Done(1) && jobs.Done(2); }), "jobs_finish_in_the_background");
    const auto first = jobs.Take(1);
    const auto second = jobs.Take(2);
    Check(first && *first == 10 && second && *second == 20, "results_are_taken_by_key");
    Check(!jobs.Pending(1) && !jobs.Take(1), "taken_result_is_gone");
  }
  {
    std::atomic<int> started{0};
    BackgroundJobs<int> jobs([&] { ++started; });
    jobs.Submit(3, [] { return 30; });
    Check(WaitFor([&] { return jobs.Done(3); }) && started == 1, "start_hook_runs_once_on_the_worker");
  }
  {
    std::atomic<bool> release{false};
    auto* jobs = new BackgroundJobs<int>([] {});
    jobs->Submit(4, [&] {
      while (!release.load()) {
        std::this_thread::yield();
      }
      return 40;
    });
    jobs->Submit(5, [] { return 50; });
    release = true;
    delete jobs;
    Check(true, "destruction_with_queued_jobs_returns");
  }
  return failures ? 1 : 0;
}
