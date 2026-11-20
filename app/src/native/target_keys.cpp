#include "target_keys.h"

#include <algorithm>
#include <array>
#include <span>

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
      const uint32_t offset = row * key.pitch_tiles + col;
      if (offset >= xenos::kEdramTileCount) {
        return;
      }
      if (!visit((key.base_tiles + offset) % xenos::kEdramTileCount)) {
        return;
      }
    }
  }
}

void EdramOwnership::Write(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t owner,
                           uint64_t sequence) {
  ForTiles(key, x0, y0, x1, y1, [&](uint32_t tile) {
    tiles_[tile] = {owner, sequence};
    return true;
  });
}

std::vector<ForeignTile> EdramOwnership::ForeignTiles(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1,
                                                      uint32_t y1, uint32_t owner) const {
  std::vector<ForeignTile> foreign;
  ForTiles(key, x0, y0, x1, y1, [&](uint32_t tile) {
    if (tiles_[tile].owner && tiles_[tile].owner != owner) {
      foreign.push_back({tile, tiles_[tile].owner, tiles_[tile].sequence});
    }
    return true;
  });
  return foreign;
}

TargetPoint TilePosition(const TargetKey& key, uint32_t tile) {
  const uint32_t offset =
      (tile + xenos::kEdramTileCount - key.base_tiles % xenos::kEdramTileCount) % xenos::kEdramTileCount;
  return {(offset % key.pitch_tiles) * xenos::kEdramTileWidthSamples,
          (offset / key.pitch_tiles) * xenos::kEdramTileHeightSamples};
}

uint32_t UsedRows(const TargetSet& set, uint32_t estimated_rows) {
  std::array<TargetKey, 5> bound;
  size_t count = 0;
  for (const auto& color : set.color) {
    if (color) {
      bound[count++] = *color;
    }
  }
  if (set.depth) {
    bound[count++] = *set.depth;
  }
  const std::span<const TargetKey> used(bound.data(), count);
  uint32_t rows = estimated_rows;
  for (const TargetKey& key : used) {
    uint32_t distance = xenos::kEdramTileCount;
    for (const TargetKey& other : used) {
      const uint32_t ahead = (other.base_tiles + xenos::kEdramTileCount - key.base_tiles) % xenos::kEdramTileCount;
      if (ahead) {
        distance = std::min(distance, ahead);
      }
    }
    rows = std::min(rows, distance / key.pitch_tiles * xenos::kEdramTileHeightSamples);
  }
  return rows;
}

}  // namespace nier::native
