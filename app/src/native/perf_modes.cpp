#include "perf_modes.h"

#include <chrono>
#include <cstddef>
#include <iterator>
#include <thread>

#include <rex/cvar.h>
#include <rex/platform.h>

REXCVAR_DEFINE_INT32(nier_perf_skip, 0, "NieR", "Native renderer: A/B skip bits forced on without the console lap");
REXCVAR_DEFINE_INT32(nier_draw_delay_us, 0, "NieR", "Native renderer: microseconds the ring thread waits on each draw to mimic the console's pace");

#if REX_PLATFORM_SWITCH
extern "C" bool RexSwitchPerfSkip(unsigned bit);
extern "C" void RexSwitchPerfUseModes(const uint32_t* masks, const char* const* names, size_t count);
#endif

namespace nier::native {

namespace {

constexpr uint32_t kModeMasks[] = {0,           kPolledWaits,   kFreshDescriptors, kRebindPipelines,
                                   kNoRawDepth, kQuarterScreen, kSkipDraws};
constexpr const char* kModeNames[] = {"normal",       "polled waits",   "fresh descriptors", "rebind pipelines",
                                      "no raw depth", "quarter screen", "no draws"};

}  // namespace

void InstallPerfModes() {
#if REX_PLATFORM_SWITCH
  RexSwitchPerfUseModes(kModeMasks, kModeNames, std::size(kModeMasks));
#endif
}

bool PerfSkip(uint32_t bit) {
  if (uint32_t(REXCVAR_GET(nier_perf_skip)) & bit) {
    return true;
  }
#if REX_PLATFORM_SWITCH
  return RexSwitchPerfSkip(bit);
#else
  return false;
#endif
}

void DelayDraw() {
  if (const int32_t delay = REXCVAR_GET(nier_draw_delay_us); delay > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(delay));
  }
}

}  // namespace nier::native
