#include <cstdint>
#include <cstdio>
#include <memory>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

#include "target_keys.h"

using namespace rex::graphics;
using nier::native::EdramOwnership;
using nier::native::ForeignTile;
using nier::native::TargetPoint;
using nier::native::TilePosition;
using nier::native::TargetSet;
using nier::native::UsedRows;
using nier::native::DownscaleShift;
using nier::native::ExtentFor;
using nier::native::TargetKey;
using nier::native::TargetsFor;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static void Setup(RegisterFile& r, uint32_t pitch, xenos::MsaaSamples msaa, xenos::EdramMode mode, uint32_t color_base,
                  uint32_t depth_base, bool depth) {
  reg::RB_SURFACE_INFO surface{};
  surface.surface_pitch = pitch;
  surface.msaa_samples = msaa;
  r[XE_GPU_REG_RB_SURFACE_INFO] = surface.value;
  reg::RB_MODECONTROL mode_control{};
  mode_control.edram_mode = mode;
  r[XE_GPU_REG_RB_MODECONTROL] = mode_control.value;
  reg::RB_COLOR_INFO color{};
  color.color_base = color_base;
  color.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  r[XE_GPU_REG_RB_COLOR_INFO] = color.value;
  reg::RB_DEPTH_INFO depth_info{};
  depth_info.depth_base = depth_base;
  depth_info.depth_format = xenos::DepthRenderTargetFormat::kD24S8;
  r[XE_GPU_REG_RB_DEPTH_INFO] = depth_info.value;
  reg::RB_DEPTHCONTROL depth_control{};
  depth_control.z_enable = depth;
  depth_control.z_write_enable = depth;
  depth_control.zfunc = xenos::CompareFunction::kLessEqual;
  r[XE_GPU_REG_RB_DEPTHCONTROL] = depth_control.value;
}

int main() {
  auto regs = std::make_unique<RegisterFile>();
  RegisterFile& r = *regs;
  {
    Setup(r, 640, xenos::MsaaSamples::k4X, xenos::EdramMode::kColorDepth, 720, 0, true);
    auto msaa = TargetsFor(r, 0xF);
    Setup(r, 1280, xenos::MsaaSamples::k1X, xenos::EdramMode::kColorDepth, 720, 0, true);
    auto single = TargetsFor(r, 0xF);
    Check(msaa.color[0] && single.color[0] && *msaa.color[0] == *single.color[0] && *msaa.depth == *single.depth,
          "msaa4x_640_equals_1x_1280");
    Check(msaa.scale.x == 2 && msaa.scale.y == 2 && single.scale.x == 1 && single.scale.y == 1, "sample_scale");
    Check(msaa.color[0]->pitch_tiles == 16 && msaa.color[0]->base_tiles == 720, "pitch_in_tiles");
  }
  {
    Setup(r, 520, xenos::MsaaSamples::k4X, xenos::EdramMode::kDepthOnly, 0, 0, true);
    auto shadow4x = TargetsFor(r, 0);
    Setup(r, 1040, xenos::MsaaSamples::k1X, xenos::EdramMode::kDepthOnly, 0, 0, true);
    auto shadow1x = TargetsFor(r, 0);
    Check(shadow4x.depth && shadow1x.depth && *shadow4x.depth == *shadow1x.depth && !shadow4x.color[0],
          "shadow_520_4x_equals_1040_1x");
  }
  {
    Setup(r, 1280, xenos::MsaaSamples::k1X, xenos::EdramMode::kColorDepth, 720, 0, false);
    auto none = TargetsFor(r, 0);
    Check(!none.color[0] && !none.depth, "color_mask_zero_has_no_color");
  }
  {
    Setup(r, 640, xenos::MsaaSamples::k4X, xenos::EdramMode::kDepthOnly, 720, 720, true);
    auto depth_only = TargetsFor(r, 0);
    Check(!depth_only.color[0] && depth_only.depth && depth_only.depth->base_tiles == 720, "depth_only_mode_has_no_color");
  }
  {
    Setup(r, 1280, xenos::MsaaSamples::k1X, xenos::EdramMode::kColorDepth, 720, 720, true);
    auto shared = TargetsFor(r, 0xF);
    TargetKey color = *shared.color[0];
    TargetKey depth = color;
    depth.is_depth = true;
    Check(shared.color[0] && !shared.depth, "depth_on_color_base_dropped");
    Check(!(color == depth), "color_and_depth_on_same_base_are_distinct");
  }
  {
    TargetKey key{720, 16, false, 0};
    auto extent = ExtentFor(key);
    TargetKey narrow{0, 1, false, 0};
    Check(extent.width == 1280 && extent.height == 2048, "extent_from_pitch");
    Check(ExtentFor(narrow).width == 80 && ExtentFor(narrow).height == 8192, "extent_clamped_to_8192");
  }
  {
    EdramOwnership edram;
    const TargetKey scene{0xB8, 8, false, 0};
    const TargetKey post{0x2D0, 16, false, 0};
    edram.Write(scene, 0, 0, 640, 368, 1, 10);
    Check(edram.ForeignTiles(scene, 0, 0, 640, 368, 1).empty(), "own_tiles_are_not_foreign");
    Check(edram.ForeignTiles(post, 0, 0, 1280, 720, 2).empty(), "disjoint_tiles_are_not_foreign");
    const TargetKey overlap{0xB8, 16, false, 0};
    const auto first_row = edram.ForeignTiles(overlap, 0, 0, 1280, 16, 3);
    Check(first_row.size() == 16 && first_row.front() == ForeignTile{0xB8, 1, 10} &&
              first_row.back() == ForeignTile{0xB8 + 15, 1, 10},
          "overlapping_tiles_listed_with_their_owner_and_write_sequence");
    edram.Write(overlap, 0, 0, 1280, 16, 3, 11);
    const auto rewritten = edram.ForeignTiles(scene, 0, 0, 640, 368, 1);
    Check(rewritten.size() == 16 && rewritten.front().owner == 3 && rewritten.front().sequence == 11,
          "later_write_through_another_target_makes_tiles_foreign");
    Check(edram.ForeignTiles(scene, 0, 32, 640, 368, 1).empty(), "rows_below_the_overwrite_stay_owned");
    const TargetKey wrapping{2040, 16, false, 0};
    edram.Write(wrapping, 0, 0, 1280, 16, 4, 12);
    const auto wrapped = edram.ForeignTiles(TargetKey{0, 8, false, 0}, 0, 0, 640, 16, 5);
    Check(wrapped.size() == 8 && wrapped.front() == ForeignTile{0, 4, 12}, "tiles_wrap_at_the_end_of_edram");
    Check(edram.ForeignTiles(scene, 0, 0, 0, 0, 1).empty(), "empty_rect_has_no_tiles");
    const TargetKey odd_pitch{0, 30, false, 0};
    edram.Write(odd_pitch, 0, 0, 2400, 8192, 6, 13);
    const auto everything = edram.ForeignTiles(odd_pitch, 0, 0, 2400, 8192, 7);
    Check(everything.size() == 2048, "a_rect_taller_than_edram_visits_each_tile_once");
  }
  {
    const TargetKey post{0x2D0, 16, false, 0};
    const TargetPoint inside = TilePosition(post, 0x2D0 + 16 * 2 + 3);
    const TargetPoint wrapped = TilePosition(TargetKey{2040, 16, false, 0}, 5);
    Check(inside.x == 3 * 80 && inside.y == 2 * 16 && wrapped.x == 13 * 80 && wrapped.y == 0,
          "tile_position_inside_a_target");
  }
  {
    TargetSet scene{};
    scene.color[0] = TargetKey{0x2D0, 16, false, 0};
    scene.depth = TargetKey{0, 16, true, 0};
    TargetSet post{};
    post.color[0] = TargetKey{0x1D4, 13, false, 0};
    TargetSet shadow{};
    shadow.depth = TargetKey{0, 13, true, 0};
    Check(UsedRows(scene, 2048) == 720, "rows_stop_where_the_next_bound_target_starts");
    Check(UsedRows(scene, 576) == 576, "rows_below_the_limit_are_kept");
    Check(UsedRows(post, 576) == 576, "single_target_keeps_estimated_rows");
    Check(UsedRows(shadow, 100000) == 2048 / 13 * 16, "rows_never_exceed_the_edram");
  }
  {
    TargetSet shadow_map{};
    shadow_map.depth = TargetKey{0, 13, true, 0};
    shadow_map.scale = {1, 1};
    TargetSet depth_clear = shadow_map;
    depth_clear.scale = {2, 2};
    TargetSet scene_1024{};
    scene_1024.color[0] = TargetKey{0x1D4, 13, false, 0};
    scene_1024.depth = TargetKey{0, 13, true, 0};
    scene_1024.scale = {1, 1};
    TargetSet scene_768{};
    scene_768.color[0] = TargetKey{0x10E, 10, false, 0};
    scene_768.depth = TargetKey{0, 10, true, 0};
    scene_768.scale = {1, 1};
    TargetSet shadow_mask{};
    shadow_mask.color[0] = TargetKey{0xB8, 8, false, 0};
    shadow_mask.depth = TargetKey{0, 8, true, 0};
    shadow_mask.scale = {1, 1};
    TargetSet depth_768{};
    depth_768.depth = TargetKey{0, 10, true, 0};
    depth_768.scale = {1, 1};
    Check(DownscaleShift(shadow_map, 1, 2) == 1, "shadow_map_pass_uses_the_map_shift");
    Check(DownscaleShift(depth_clear, 1, 2) == 0, "multisampled_depth_only_pass_keeps_full_size");
    Check(DownscaleShift(scene_1024, 1, 2) == 0, "scene_with_the_shadow_map_depth_keeps_full_size");
    Check(DownscaleShift(scene_768, 1, 2) == 0, "scene_keeps_full_size");
    Check(DownscaleShift(shadow_mask, 1, 2) == 2, "shadow_mask_pass_uses_the_mask_shift");
    Check(DownscaleShift(depth_768, 1, 2) == 0, "narrow_depth_only_pass_keeps_full_size");
    Check(DownscaleShift(shadow_map, 0, 0) == 0, "zero_shifts_turn_it_off");
  }
  return failures ? 1 : 0;
}
