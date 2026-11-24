#include <cstdio>
#include <string>

#include "nier_npc_lod.h"

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  Check(NierNpcSizeBin(0.01) == 0 && NierNpcSizeBin(0.03) == 0, "small_sizes_in_first_bin");
  Check(NierNpcSizeBin(0.035) == 1 && NierNpcSizeBin(0.05) == 2 && NierNpcSizeBin(0.055) == 3, "middle_bins");
  Check(NierNpcSizeBin(0.5) == 4, "large_size_in_last_bin");
  Check(NierNpcSizeBin(kNierNpcSizeSentinel) == 5 && NierNpcSizeBin(999999.0) == 5, "sentinels_counted_apart");
  Check(NierNpcScaledSize(0.05, 0.6) == 0.05 * 0.6, "size_scaled");
  Check(NierNpcScaledSize(kNierNpcSizeSentinel, 0.6) == kNierNpcSizeSentinel &&
            NierNpcScaledSize(999999.0, 0.5) == 999999.0,
        "sentinels_kept");
  NierNpcCount(0.02);
  NierNpcCount(0.045);
  NierNpcCount(kNierNpcSizeSentinel);
  const std::string report = NierNpcLodReport(0.6);
  Check(report == "npc lod: scale 0.60, 3 size checks, <=0.03 1, <=0.04 0, <=0.05 1, <=0.06 0, larger 0, always 1",
        "report_counts_since_last");
  Check(NierNpcLodReport(0.6).find("0 size checks") != std::string::npos, "report_resets");
  return failures ? 1 : 0;
}
