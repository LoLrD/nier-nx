#pragma once

#include <cstdint>
#include <map>

#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

#include "target_keys.h"

namespace nier::native {

struct ResolvePlan {
  bool copy;
  bool depth;
  TargetKey source;
  uint32_t source_x;
  uint32_t source_y;
  uint32_t source_width;
  uint32_t source_height;
  uint32_t dest_address;
  rex::graphics::xenos::TextureFormat dest_format;
  rex::graphics::xenos::Endian128 dest_endian;
  bool dest_swap;
  uint32_t dest_x;
  uint32_t dest_y;
  uint32_t dest_width;
  uint32_t dest_height;
  uint32_t dest_pitch;
  uint32_t dest_texture_height;
  bool readback;
  bool clear_color;
  bool clear_depth;
  TargetKey depth_target;
  float color_clear[4];
  float depth_clear;
  uint32_t stencil_clear;
};

struct PendingRange {
  uint32_t address;
  uint32_t bytes;
  uint64_t guest_hash;
};

ResolvePlan PlanResolve(const rex::graphics::draw_util::ResolveInfo& info, const rex::graphics::RegisterFile& regs);
uint32_t ResolveGuestBytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel, uint32_t address);
bool PresentableResolve(const ResolvePlan& plan);
bool SameColorCopy(const ResolvePlan& earlier, const ResolvePlan& later);
bool ShouldWriteBack(const PendingRange& range, uint64_t current_hash);
void CarryWrittenHash(PendingRange& later, const PendingRange& written, uint64_t written_hash);

enum class StaleRead : uint8_t { kNone, kOlderImage, kForgotten, kRemoved, kInsideRange };

constexpr uint64_t kRecentResolveFrames = 600;

struct ResolveRecord {
  uint32_t address;
  uint32_t bytes;
  uint64_t frame;
  uint32_t format;
  StaleRead lost;
  uint32_t width = 0;
  uint32_t height = 0;
};

StaleRead GuestReadKind(const std::map<uint32_t, ResolveRecord>& records, uint32_t address, uint64_t frame);
bool OlderThanNewest(const std::map<uint32_t, ResolveRecord>& records, uint32_t address, uint64_t published_frame);
void WriteTiled(const uint8_t* host_rgba, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                uint32_t pitch_aligned, rex::graphics::xenos::Endian128 endian, bool swap_red_blue, uint8_t* guest);

}  // namespace nier::native
