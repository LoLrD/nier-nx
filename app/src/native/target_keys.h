#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace nier::native {

struct SampleScale {
  uint32_t x;
  uint32_t y;
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
};

SampleScale ScaleFor(rex::graphics::xenos::MsaaSamples msaa);
TargetExtent ExtentFor(const TargetKey& key);
TargetSet TargetsFor(const rex::graphics::RegisterFile& regs, uint32_t normalized_color_mask);

class EdramOwnership {
 public:
  void Write(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t owner);
  uint32_t ForeignOwner(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                        uint32_t owner) const;

 private:
  template <typename Visit>
  static void ForTiles(const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, Visit visit);

  std::array<uint32_t, rex::graphics::xenos::kEdramTileCount> owners_{};
};

}  // namespace nier::native
