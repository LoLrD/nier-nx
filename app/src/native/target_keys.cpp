#include "target_keys.h"

#include <algorithm>

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>

namespace nier::native {

using namespace rex::graphics;

namespace {

constexpr uint32_t kMaxHeightSamples = 8192;

}  // namespace

size_t TargetKeyHash::operator()(const TargetKey& key) const {
  return size_t(key.base_tiles) | size_t(key.pitch_tiles) << 12 | size_t(key.is_depth) << 24 |
         size_t(key.format) << 25;
}

SampleScale ScaleFor(xenos::MsaaSamples msaa) {
  return {msaa >= xenos::MsaaSamples::k4X ? 2u : 1u, msaa >= xenos::MsaaSamples::k2X ? 2u : 1u};
}

TargetExtent ExtentFor(const TargetKey& key) {
  const uint32_t rows = (xenos::kEdramTileCount + key.pitch_tiles - 1) / key.pitch_tiles;
  return {key.pitch_tiles * xenos::kEdramTileWidthSamples,
          std::min(rows, kMaxHeightSamples / xenos::kEdramTileHeightSamples) * xenos::kEdramTileHeightSamples};
}

TargetSet TargetsFor(const RegisterFile& regs, uint32_t normalized_color_mask) {
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  TargetSet set{};
  set.scale = ScaleFor(surface.msaa_samples);
  const uint32_t pitch_tiles =
      ((surface.surface_pitch << (set.scale.x - 1)) + xenos::kEdramTileWidthSamples - 1) /
      xenos::kEdramTileWidthSamples;
  if (!pitch_tiles) {
    return set;
  }
  const auto depth_control = draw_util::GetNormalizedDepthControl(regs);
  if (depth_control.z_enable || depth_control.stencil_enable) {
    const auto depth = regs.Get<reg::RB_DEPTH_INFO>();
    set.depth = TargetKey{depth.depth_base, pitch_tiles, true, uint32_t(depth.depth_format)};
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(normalized_color_mask & (0xFu << (4 * i)))) {
      continue;
    }
    const auto color = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    set.color[i] = TargetKey{color.color_base, pitch_tiles, false,
                             uint32_t(xenos::GetStorageColorFormat(color.color_format))};
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!set.color[i]) {
      continue;
    }
    for (uint32_t j = i + 1; j < xenos::kMaxColorRenderTargets; ++j) {
      if (set.color[j] && set.color[j]->base_tiles == set.color[i]->base_tiles) {
        set.color[j].reset();
      }
    }
    if (set.depth && set.depth->base_tiles == set.color[i]->base_tiles) {
      set.depth.reset();
    }
  }
  return set;
}

template <typename Visit>
void EdramOwnership::ForTiles(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, Visit visit) {
  const uint32_t col_end =
      std::min((x1 + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples, key.pitch_tiles);
  const uint32_t row_end = (y1 + xenos::kEdramTileHeightSamples - 1) / xenos::kEdramTileHeightSamples;
  for (uint32_t row = y0 / xenos::kEdramTileHeightSamples; row < row_end; ++row) {
    for (uint32_t col = x0 / xenos::kEdramTileWidthSamples; col < col_end; ++col) {
      if (!visit((key.base_tiles + row * key.pitch_tiles + col) % xenos::kEdramTileCount)) {
        return;
      }
    }
  }
}

void EdramOwnership::Write(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t owner) {
  ForTiles(key, x0, y0, x1, y1, [&](uint32_t tile) {
    owners_[tile] = owner;
    return true;
  });
}

uint32_t EdramOwnership::ForeignOwner(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                                      uint32_t owner) const {
  uint32_t foreign = 0;
  ForTiles(key, x0, y0, x1, y1, [&](uint32_t tile) {
    if (owners_[tile] && owners_[tile] != owner) {
      foreign = owners_[tile];
      return false;
    }
    return true;
  });
  return foreign;
}

}  // namespace nier::native
