#include "resolve_plan.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/math.h>

#include "texture_loader.h"
#include "tiling.h"

namespace nier::native {

using namespace rex::graphics;

namespace {

constexpr uint32_t kReadbackLimitBytes = 256 * 1024;
constexpr uint64_t kPhysicalBytes = 0x20000000ull;

xenos::Endian Endian32(xenos::Endian128 endian) {
  switch (endian) {
    case xenos::Endian128::kNone:
      return xenos::Endian::kNone;
    case xenos::Endian128::k8in16:
      return xenos::Endian::k8in16;
    case xenos::Endian128::k16in32:
      return xenos::Endian::k16in32;
    default:
      return xenos::Endian::k8in32;
  }
}

}  // namespace

ResolvePlan PlanResolve(const draw_util::ResolveInfo& info, const RegisterFile& regs) {
  ResolvePlan plan{};
  plan.depth = info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo edram = plan.depth ? info.depth_edram_info : info.color_edram_info;
  const uint32_t original_base = plan.depth ? info.depth_original_base : info.color_original_base;
  const SampleScale scale = ScaleFor(edram.msaa_samples);
  const uint32_t format =
      plan.depth ? edram.format
                 : uint32_t(xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(edram.format)));
  plan.source = TargetKey{original_base, edram.pitch_tiles, plan.depth, format};
  const uint32_t delta = edram.base_tiles - original_base;
  const uint32_t log2_x = scale.x - 1;
  const uint32_t log2_y = scale.y - 1;
  plan.source_x = (delta % edram.pitch_tiles) * xenos::kEdramTileWidthSamples +
                  (uint32_t(info.coordinate_info.edram_offset_x_div_8) << (log2_x + 3));
  plan.source_y = (delta / edram.pitch_tiles) * xenos::kEdramTileHeightSamples +
                  (uint32_t(info.coordinate_info.edram_offset_y_div_8) << (log2_y + 3));
  plan.dest_width = uint32_t(info.coordinate_info.width_div_8) << 3;
  plan.dest_height = info.height_div_8 << 3;
  plan.source_width = plan.dest_width << log2_x;
  plan.source_height = plan.dest_height << log2_y;
  plan.dest_x = plan.source_x >> log2_x;
  plan.dest_y = plan.source_y >> log2_y;
  plan.dest_address = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  plan.dest_format = xenos::TextureFormat(info.copy_dest_info.copy_dest_format);
  plan.dest_endian = info.copy_dest_info.copy_dest_endian;
  plan.dest_swap = info.copy_dest_info.copy_dest_swap;
  const auto pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  plan.dest_pitch = pitch.copy_dest_pitch;
  plan.dest_texture_height = pitch.copy_dest_height;
  plan.copy = HostFormatFor(plan.dest_format).has_value() && plan.dest_width && plan.dest_height;
  const uint32_t bytes_per_pixel = FormatInfo::Get(plan.dest_format)->bits_per_pixel / 8;
  plan.readback = plan.copy && !plan.depth && bytes_per_pixel == 4 &&
                  plan.dest_pitch * plan.dest_texture_height * bytes_per_pixel <= kReadbackLimitBytes &&
                  plan.dest_width * plan.dest_height * bytes_per_pixel <= kReadbackLimitBytes;
  plan.clear_color = info.IsClearingColor();
  plan.clear_depth = info.IsClearingDepth();
  if (plan.clear_depth) {
    plan.depth_target = TargetKey{info.depth_original_base, info.depth_edram_info.pitch_tiles, true,
                                  info.depth_edram_info.format};
  }
  for (uint32_t i = 0; i < 4; ++i) {
    plan.color_clear[i] = float((info.rb_color_clear >> (8 * i)) & 0xFF) * (1.0f / 255.0f);
  }
  plan.depth_clear = xenos::UNorm24To32((info.rb_depth_clear >> 8) & 0xFFFFFF);
  plan.stencil_clear = info.rb_depth_clear & 0xFF;
  return plan;
}

uint32_t ResolveGuestBytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel, uint32_t address) {
  const uint64_t bytes = uint64_t(rex::align(width, 32u)) * rex::align(height, 32u) * bytes_per_pixel;
  return uint32_t(std::min(bytes, kPhysicalBytes - std::min<uint64_t>(address, kPhysicalBytes)));
}

bool PresentableResolve(const ResolvePlan& plan) {
  return plan.copy && !plan.depth && plan.dest_format == xenos::TextureFormat::k_8_8_8_8;
}

bool ShouldWriteBack(const PendingRange& range, uint64_t current_hash) { return range.guest_hash == current_hash; }

void CarryWrittenHash(PendingRange& later, const PendingRange& written, uint64_t written_hash) {
  if (later.address == written.address && later.bytes == written.bytes && later.guest_hash == written.guest_hash) {
    later.guest_hash = written_hash;
  }
}

void WriteTiled(const uint8_t* host_rgba, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                uint32_t pitch_aligned, xenos::Endian128 endian, bool swap_red_blue, uint8_t* guest) {
  const xenos::Endian swap = Endian32(endian);
  for (uint32_t row = 0; row < height; ++row) {
    const uint8_t* source = host_rgba + size_t(row) * width * 4;
    for (uint32_t column = 0; column < width;) {
      const uint32_t guest_x = x + column;
      uint8_t* target = guest + TiledOffset2D(int32_t(guest_x), int32_t(y + row), pitch_aligned, 2);
      const uint32_t run = guest_x & 3 ? 1 : std::min(4u, width - column);
      for (uint32_t i = 0; i < run; ++i, ++column) {
        uint8_t texel[4];
        std::memcpy(texel, source + size_t(column) * 4, 4);
        if (swap_red_blue) {
          std::swap(texel[0], texel[2]);
        }
        uint32_t value;
        std::memcpy(&value, texel, 4);
        value = xenos::GpuSwap(value, swap);
        std::memcpy(target + i * 4, &value, 4);
      }
    }
  }
}

StaleRead GuestReadKind(const std::map<uint32_t, ResolveRecord>& records, uint32_t address, uint64_t frame) {
  auto it = records.upper_bound(address);
  if (it == records.begin()) {
    return StaleRead::kNone;
  }
  const ResolveRecord& record = std::prev(it)->second;
  if (address - record.address >= record.bytes || frame - record.frame > kRecentResolveFrames) {
    return StaleRead::kNone;
  }
  return record.address == address ? record.lost : StaleRead::kInsideRange;
}

bool OlderThanNewest(const std::map<uint32_t, ResolveRecord>& records, uint32_t address, uint64_t published_frame) {
  const auto it = records.find(address);
  return it != records.end() && published_frame < it->second.frame;
}

}  // namespace nier::native
