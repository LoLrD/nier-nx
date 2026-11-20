#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "gpu_times.h"

using namespace nier::native;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  const uint64_t ticks[] = {0, 100, 400, 450, 1000};
  const GpuCategory categories[] = {GpuCategory::kUploads, GpuCategory::kDraws, GpuCategory::kResolves,
                                    GpuCategory::kDraws, GpuCategory::kNone};
  {
    std::array<uint64_t, kGpuCategories> ns{};
    AddGpuTimes(ticks, categories, 5, 1.0, ~0ull, ns);
    Check(ns[size_t(GpuCategory::kUploads)] == 100 && ns[size_t(GpuCategory::kDraws)] == 850 &&
              ns[size_t(GpuCategory::kResolves)] == 50 && ns[size_t(GpuCategory::kPresent)] == 0,
          "intervals_go_to_the_category_that_starts_them");
  }
  {
    std::array<uint64_t, kGpuCategories> ns{};
    const GpuCategory gap[] = {GpuCategory::kUploads, GpuCategory::kNone, GpuCategory::kPresent, GpuCategory::kNone,
                               GpuCategory::kNone};
    AddGpuTimes(ticks, gap, 5, 2.0, ~0ull, ns);
    Check(ns[size_t(GpuCategory::kUploads)] == 200 && ns[size_t(GpuCategory::kPresent)] == 100 &&
              ns[size_t(GpuCategory::kDraws)] == 0,
          "none_intervals_dropped_and_period_scales");
  }
  {
    std::array<uint64_t, kGpuCategories> ns{};
    const uint64_t wrapped[] = {0xFFFFFFF0ull, 0x10ull};
    const GpuCategory pair[] = {GpuCategory::kDraws, GpuCategory::kNone};
    AddGpuTimes(wrapped, pair, 2, 1.0, 0xFFFFFFFFull, ns);
    Check(ns[size_t(GpuCategory::kDraws)] == 0x20, "ticks_wrap_inside_valid_bits");
  }
  {
    std::array<uint64_t, kGpuCategories> ns{};
    AddGpuTimes(ticks, categories, 1, 1.0, ~0ull, ns);
    Check(ns[size_t(GpuCategory::kUploads)] == 0, "single_mark_adds_nothing");
  }
  {
    std::array<uint64_t, kGpuCategories> ns{};
    const GpuCategory interrupted[] = {GpuCategory::kDraws, GpuCategory::kTransfers, GpuCategory::kDraws,
                                       GpuCategory::kTransfers, GpuCategory::kNone};
    AddGpuTimes(ticks, interrupted, 5, 1.0, ~0ull, ns);
    Check(ns[size_t(GpuCategory::kTransfers)] == 300 + 550 && ns[size_t(GpuCategory::kDraws)] == 100 + 50,
          "transfers_inside_a_pass_are_their_own_category");
  }
  {
    std::vector<uint64_t> ns;
    const uint16_t passes[] = {0, 2, 1, 2, 0};
    AddPassTimes(ticks, passes, 5, 2.0, ~0ull, ns);
    Check(ns.size() == 3 && ns[0] == 0 && ns[1] == 100 && ns[2] == 600 + 1100, "pass_intervals_go_to_their_pass");
  }
  return failures ? 1 : 0;
}
