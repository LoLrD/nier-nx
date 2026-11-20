#pragma once

#include <cstdint>

namespace nier::native {

constexpr uint32_t kSkipDraws = 1;
constexpr uint32_t kSkipPixels = 2048;
constexpr uint32_t kSkipPixelShaders = 4096;
constexpr uint32_t kQuarterScreen = 8192;
constexpr uint32_t kGenericShaders = 16384;
constexpr uint32_t kExplicitGradients = 32768;
constexpr uint32_t kExactMath = 65536;
constexpr uint32_t kPolledWaits = 131072;
constexpr uint32_t kFreshDescriptors = 262144;
constexpr uint32_t kRebindPipelines = 524288;

void InstallPerfModes();
bool PerfSkip(uint32_t bit);
void DelayDraw();

}  // namespace nier::native
