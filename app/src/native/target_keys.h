#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace nier::native {

struct SampleScale {
  uint32_t x;
  uint32_t y;

  bool operator==(const SampleScale&) const = default;
};

struct TargetKey {
  uint32_t base_tiles;
  uint32_t pitch_tiles;
  bool is_depth;
  uint32_t format;

  bool operator==(const TargetKey&) const = default;
};

struct TargetKeyHash {
  size_t operator()(const TargetKey& key) const;
};

struct TargetExtent {
  uint32_t width;
  uint32_t height;
};

struct TargetSet {
  std::array<std::optional<TargetKey>, 4> color;
  std::optional<TargetKey> depth;
  SampleScale scale;

  bool operator==(const TargetSet&) const = default;
};

enum class PassKind : uint8_t { kOther, kShadowMap, kScreen640 };

SampleScale ScaleFor(rex::graphics::xenos::MsaaSamples msaa);
TargetExtent ExtentFor(const TargetKey& key);
TargetSet TargetsFor(const rex::graphics::RegisterFile& regs, uint32_t normalized_color_mask);

struct ForeignTile {
  uint32_t tile;
  uint32_t owner;
  uint64_t sequence;

  bool operator==(const ForeignTile&) const = default;
};

struct TargetPoint {
  uint32_t x;
  uint32_t y;
};

TargetPoint TilePosition(const TargetKey& key, uint32_t tile);
uint32_t UsedRows(const TargetSet& set, uint32_t estimated_rows);
PassKind KindOf(const TargetSet& set);

struct ScaledSpan {
  uint32_t begin;
  uint32_t length;
};

ScaledSpan ScaledRange(uint32_t begin, uint32_t length, uint32_t shift, uint32_t limit);

class EdramOwnership {
 public:
  void Write(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t owner,
             uint64_t sequence);
  std::vector<ForeignTile> ForeignTiles(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                                        uint32_t owner) const;

 private:
  template <typename Visit>
  static void ForTiles(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, Visit visit);

  struct Tile {
    uint32_t owner = 0;
    uint64_t sequence = 0;
  };

  std::array<Tile, rex::graphics::xenos::kEdramTileCount> tiles_{};
};

}  // namespace nier::native
