#include <cstdint>
#include <cstdio>
#include <memory>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

#include "target_keys.h"

using namespace rex::graphics;
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
  return failures ? 1 : 0;
}
