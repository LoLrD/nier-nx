#include <cstdint>
#include <cstdio>
#include <vector>

#include "phased_hash.h"

using nier::native::HashSpan;
using nier::native::PhasedHash;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static void Settle(PhasedHash& hash, const HashSpan& span) {
  for (uint32_t i = 0; i < PhasedHash::kStableChecks; ++i) {
    hash.Check({&span, 1});
  }
}

int main() {
  constexpr uint32_t kBytes = 64 * PhasedHash::kBlockBytes;
  std::vector<uint8_t> memory(kBytes, 0x11);
  const HashSpan span{memory.data(), kBytes};
  {
    PhasedHash hash;
    hash.Reset({&span, 1});
    const auto unchanged = hash.Check({&span, 1});
    memory[100] = 0x22;
    const auto changed = hash.Check({&span, 1});
    memory[100] = 0x11;
    Check(!unchanged.changed && unchanged.hashed_bytes == kBytes, "unproven_content_checked_in_full");
    Check(changed.changed, "change_found_by_full_check");
  }
  {
    PhasedHash hash;
    hash.Reset({&span, 1});
    Settle(hash, span);
    const auto sampled = hash.Check({&span, 1});
    Check(!sampled.changed && sampled.sampled &&
              sampled.hashed_bytes <= kBytes / PhasedHash::kPhases + 2 * PhasedHash::kBlockBytes,
          "stable_content_checked_by_one_phase");
  }
  {
    PhasedHash hash;
    hash.Reset({&span, 1});
    Settle(hash, span);
    for (uint32_t i = 0; i < kBytes; i += 512) {
      memory[i] = 0x33;
    }
    const auto rewritten = hash.Check({&span, 1});
    Check(rewritten.changed, "broad_rewrite_found_by_next_check");
    Check(rewritten.sampled && !rewritten.possibly_late, "broad_rewrite_not_counted_late");
    for (uint32_t i = 0; i < kBytes; i += 512) {
      memory[i] = 0x11;
    }
  }
  {
    PhasedHash hash;
    hash.Reset({&span, 1});
    Settle(hash, span);
    memory[20 * PhasedHash::kBlockBytes + 7] = 0x44;
    bool found = false;
    bool late = false;
    for (uint32_t i = 0; i < PhasedHash::kPhases && !found; ++i) {
      const auto result = hash.Check({&span, 1});
      found = result.changed;
      late = result.possibly_late;
    }
    Check(found, "single_block_change_found_within_all_phases");
    Check(late, "single_block_change_counted_possibly_late");
    const auto after = hash.Check({&span, 1});
    Check(!after.changed && after.hashed_bytes == kBytes, "changed_content_stays_on_full_checks");
    memory[20 * PhasedHash::kBlockBytes + 7] = 0x11;
  }
  {
    PhasedHash hash;
    hash.Reset({&span, 1});
    Settle(hash, span);
    memory[kBytes - 1] = 0x55;
    Check(hash.Check({&span, 1}).changed, "last_block_checked_every_time");
    memory[kBytes - 1] = 0x11;
  }
  {
    std::vector<uint8_t> small(PhasedHash::kMinSampledBytes, 0x11);
    const HashSpan small_span{small.data(), uint32_t(small.size())};
    PhasedHash hash;
    hash.Reset({&small_span, 1});
    Settle(hash, small_span);
    Check(hash.Check({&small_span, 1}).hashed_bytes == small.size(), "small_content_always_checked_in_full");
  }
  {
    std::vector<uint8_t> mips(kBytes, 0x11);
    const HashSpan spans[2] = {span, {mips.data(), kBytes}};
    PhasedHash two;
    two.Reset(spans);
    for (uint32_t i = 0; i < PhasedHash::kStableChecks; ++i) {
      two.Check(spans);
    }
    mips[kBytes / 2] = 0x66;
    bool found = false;
    for (uint32_t i = 0; i < PhasedHash::kPhases && !found; ++i) {
      found = two.Check(spans).changed;
    }
    Check(found, "change_in_second_span_found");
  }
  {
    PhasedHash hash;
    hash.Reset({&span, 1});
    PhasedHash marked;
    marked.Reset({&span, 1});
    marked.MarkVolatile();
    Settle(hash, span);
    Settle(marked, span);
    Check(marked.Check({&span, 1}).hashed_bytes == kBytes && hash.sampling() && !marked.sampling(),
          "volatile_content_never_sampled");
  }
  {
    PhasedHash hash;
    hash.Reset({&span, 1});
    Check(hash.Matches({&span, 1}), "matches_unchanged_content");
    memory[300] = 0x77;
    Check(!hash.Matches({&span, 1}), "matches_detects_change");
    memory[300] = 0x11;
  }
  return failures ? 1 : 0;
}
