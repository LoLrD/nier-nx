#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>

#include "resolve_plan.h"

using namespace rex::graphics;
using namespace nier::native;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static draw_util::ResolveInfo ColorResolve(uint32_t width, uint32_t height) {
  draw_util::ResolveInfo info{};
  info.rb_copy_control.copy_src_select = 0;
  info.rb_copy_control.copy_command = xenos::CopyCommand::kConvert;
  info.color_edram_info.pitch_tiles = 16;
  info.color_edram_info.msaa_samples = xenos::MsaaSamples::k1X;
  info.color_edram_info.base_tiles = 720;
  info.color_edram_info.format = uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  info.color_original_base = 720;
  info.coordinate_info.width_div_8 = width / 8;
  info.height_div_8 = height / 8;
  info.copy_dest_info.copy_dest_format = xenos::ColorFormat(xenos::TextureFormat::k_8_8_8_8);
  info.copy_dest_info.copy_dest_endian = xenos::Endian128::k8in32;
  return info;
}

static void Dest(RegisterFile& r, uint32_t base, uint32_t pitch, uint32_t height) {
  r[XE_GPU_REG_RB_COPY_DEST_BASE] = base;
  reg::RB_COPY_DEST_PITCH dest{};
  dest.copy_dest_pitch = pitch;
  dest.copy_dest_height = height;
  r[XE_GPU_REG_RB_COPY_DEST_PITCH] = dest.value;
}

int main() {
  auto regs = std::make_unique<RegisterFile>();
  RegisterFile& r = *regs;
  {
    Dest(r, 0x0A000000, 1280, 720);
    ResolvePlan plan = PlanResolve(ColorResolve(1280, 720), r);
    Check(plan.copy && !plan.depth && plan.source == TargetKey{720, 16, false, 0}, "fullscreen_resolve_source");
    Check(plan.source_x == 0 && plan.source_y == 0 && plan.source_width == 1280 && plan.source_height == 720 &&
              plan.dest_x == 0 && plan.dest_y == 0 && plan.dest_width == 1280 && plan.dest_height == 720,
          "fullscreen_resolve_rects");
    Check(plan.dest_address == 0x0A000000 && plan.dest_pitch == 1280 && plan.dest_texture_height == 720 &&
              plan.dest_format == xenos::TextureFormat::k_8_8_8_8 && !plan.readback,
          "fullscreen_1280_resolve_plan");
  }
  {
    Dest(r, 0x0B000000, 32, 8);
    ResolvePlan plan = PlanResolve(ColorResolve(32, 8), r);
    Check(plan.copy && plan.readback && plan.dest_width == 32 && plan.dest_height == 8, "exposure_32x8_is_read_back");
  }
  {
    Dest(r, 0x0B100000, 32, 8);
    ResolvePlan plan = PlanResolve(ColorResolve(512, 256), r);
    Check(!plan.readback, "readback_needs_rectangle_within_limit");
  }
  {
    draw_util::ResolveInfo info = ColorResolve(64, 64);
    info.color_edram_info.base_tiles = 720 + 16 * 2 + 1;
    info.coordinate_info.edram_offset_x_div_8 = 2;
    info.coordinate_info.edram_offset_y_div_8 = 1;
    Dest(r, 0x0C000000, 256, 256);
    ResolvePlan plan = PlanResolve(info, r);
    Check(plan.source_x == 96 && plan.source_y == 40 && plan.dest_x == 96 && plan.dest_y == 40,
          "offset_rect_from_tiles");
  }
  {
    draw_util::ResolveInfo info = ColorResolve(64, 64);
    info.color_edram_info.msaa_samples = xenos::MsaaSamples::k4X;
    info.color_edram_info.pitch_tiles = 16;
    Dest(r, 0x0D000000, 64, 64);
    ResolvePlan plan = PlanResolve(info, r);
    Check(plan.source_width == 128 && plan.source_height == 128 && plan.dest_width == 64, "msaa_source_in_samples");
  }
  {
    draw_util::ResolveInfo info{};
    info.rb_copy_control.copy_src_select = 4;
    info.rb_copy_control.copy_command = xenos::CopyCommand::kRaw;
    info.depth_edram_info.pitch_tiles = 16;
    info.depth_edram_info.is_depth = 1;
    info.depth_edram_info.base_tiles = 0;
    info.depth_edram_info.format = uint32_t(xenos::DepthRenderTargetFormat::kD24S8);
    info.depth_original_base = 0;
    info.coordinate_info.width_div_8 = 160;
    info.height_div_8 = 90;
    info.copy_dest_info.copy_dest_format = xenos::ColorFormat(xenos::TextureFormat::k_24_8);
    Dest(r, 0x0E000000, 1280, 720);
    ResolvePlan plan = PlanResolve(info, r);
    Check(plan.copy && plan.depth && plan.source == TargetKey{0, 16, true, 0} &&
              plan.dest_format == xenos::TextureFormat::k_24_8,
          "depth_resolve_marks_depth");
  }
  {
    draw_util::ResolveInfo info = ColorResolve(1280, 720);
    info.rb_copy_control.color_clear_enable = 1;
    info.rb_copy_control.depth_clear_enable = 1;
    info.depth_edram_info.pitch_tiles = 16;
    info.depth_edram_info.is_depth = 1;
    info.depth_edram_info.base_tiles = 0;
    info.depth_original_base = 0;
    info.rb_color_clear = 0xFF102030;
    info.rb_depth_clear = 0xFFFFFF07;
    Dest(r, 0x0A000000, 1280, 720);
    ResolvePlan plan = PlanResolve(info, r);
    Check(plan.clear_color && plan.clear_depth && plan.depth_target == TargetKey{0, 16, true, 0} &&
              plan.color_clear[0] == 0x30 * (1.0f / 255.0f) && plan.color_clear[2] == 0x10 * (1.0f / 255.0f) &&
              plan.color_clear[3] == 1.0f && plan.depth_clear == 1.0f && plan.stencil_clear == 7,
          "clear_flags_from_copy_control");
  }
  {
    std::vector<uint8_t> guest(64 * 1024, 0);
    std::vector<uint8_t> host(8 * 4 * 4);
    for (uint32_t i = 0; i < host.size(); ++i) {
      host[i] = uint8_t(i);
    }
    WriteTiled(host.data(), 4, 2, 8, 4, 32, xenos::Endian128::kNone, false, guest.data());
    uint32_t written, expected;
    std::memcpy(&written, guest.data() + texture_util::GetTiledOffset2D(4 + 3, 2 + 1, 32, 2), 4);
    std::memcpy(&expected, host.data() + (1 * 8 + 3) * 4, 4);
    Check(written == expected, "write_tiled_matches_tiled_offset");
    WriteTiled(host.data(), 0, 0, 8, 4, 32, xenos::Endian128::k8in32, false, guest.data());
    std::memcpy(&written, guest.data() + texture_util::GetTiledOffset2D(3, 1, 32, 2), 4);
    Check(written == __builtin_bswap32(expected), "write_tiled_swaps_8in32");
    WriteTiled(host.data(), 0, 0, 8, 4, 32, xenos::Endian128::kNone, true, guest.data());
    const uint8_t* texel = guest.data() + texture_util::GetTiledOffset2D(3, 1, 32, 2);
    const uint8_t* source = host.data() + (1 * 8 + 3) * 4;
    Check(texel[0] == source[2] && texel[2] == source[0] && texel[1] == source[1], "write_tiled_swaps_red_blue");
  }
  {
    Check(ResolveGuestBytes(1280, 720, 1, 0x0A000000) == 1280u * 736u &&
              ResolveGuestBytes(1280, 720, 4, 0x0A000000) == 1280u * 736u * 4u &&
              ResolveGuestBytes(100, 10, 2, 0) == 128u * 32u * 2u,
          "guest_bytes_by_bpp");
    Check(ResolveGuestBytes(1280, 720, 4, 0x1FFFF000) == 0x1000u, "guest_bytes_clipped_at_end_of_memory");
  }
  {
    Dest(r, 0x0A000000, 1280, 720);
    const ResolvePlan color = PlanResolve(ColorResolve(1280, 720), r);
    auto single = ColorResolve(1280, 720);
    single.copy_dest_info.copy_dest_format = xenos::ColorFormat(xenos::TextureFormat::k_8);
    const ResolvePlan red = PlanResolve(single, r);
    ResolvePlan depth = color;
    depth.depth = true;
    ResolvePlan skipped = color;
    skipped.copy = false;
    Check(PresentableResolve(color) && !PresentableResolve(red) && !PresentableResolve(depth) &&
              !PresentableResolve(skipped),
          "only_colour_rgba8_presentable");
  }
  {
    Check(ShouldWriteBack(PendingRange{0x1000, 256, 11}, 11) && !ShouldWriteBack(PendingRange{0x1000, 256, 11}, 12),
          "changed_guest_range_not_written");
    std::vector<PendingRange> later = {{0x1000, 256, 11}, {0x2000, 256, 11}, {0x1000, 256, 99}, {0x1000, 512, 11}};
    for (PendingRange& range : later) {
      CarryWrittenHash(range, PendingRange{0x1000, 256, 11}, 33);
    }
    Check(later[0].guest_hash == 33 && later[1].guest_hash == 11 && later[2].guest_hash == 99 &&
              later[3].guest_hash == 11,
          "later_readback_to_same_range_expects_written_bytes");
  }
  return failures ? 1 : 0;
}
