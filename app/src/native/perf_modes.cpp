#include "perf_modes.h"

#include <cstddef>
#include <iterator>

#include <rex/cvar.h>
#include <rex/platform.h>

REXCVAR_DEFINE_INT32(nier_perf_skip, 0, "NieR", "Native renderer: A/B skip bits forced on without the console lap");

#if REX_PLATFORM_SWITCH
extern "C" bool RexSwitchPerfSkip(unsigned bit);
extern "C" void RexSwitchPerfUseModes(const uint32_t* masks, const char* const* names, size_t count);
#endif

namespace nier::native {

namespace {

constexpr uint32_t kModeMasks[] = {0, kSkipPixelShaders, kQuarterScreen, kSkipPixels, kSkipDraws};
constexpr const char* kModeNames[] = {"normal", "no pixel shaders", "quarter screen", "no draw pixels", "no draws"};

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

}  // namespace nier::native
