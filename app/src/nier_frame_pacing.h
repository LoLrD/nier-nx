#pragma once

#include <array>
#include <cstdint>
#include <string>

uint32_t NierSoftenSwapParam(uint32_t param, int32_t threshold, int32_t max_interval);

enum class NierSwapOutcome : uint8_t { kOnTime, kLateShownNow, kLateNextVblank };

NierSwapOutcome NierClassifySwap(uint32_t last_target, uint32_t interval, uint32_t vblank_count, bool released_now);
void NierCountSwap(uint32_t requested_interval, NierSwapOutcome outcome);
std::string NierFramePacingReport();
