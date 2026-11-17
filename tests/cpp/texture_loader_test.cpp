#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "texture_loader.h"

using namespace rex::graphics;
using namespace nier::native;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static xenos::xe_gpu_texture_fetch_t Fetch(xenos::TextureFormat format, uint32_t width, uint32_t height, bool tiled,
                                           uint32_t base_address, xenos::DataDimension dimension) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memset(&fetch, 0, sizeof(fetch));
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.format = format;
  fetch.dimension = dimension;
  fetch.size_2d.width = width - 1;
  fetch.size_2d.height = height - 1;
  fetch.size_2d.stack_depth = dimension == xenos::DataDimension::kCube ? 5 : 0;
  fetch.pitch = (width + 31) / 32;
  fetch.tiled = tiled;
  fetch.base_address = base_address >> 12;
  return fetch;
}

static texture_util::TextureGuestLayout Layout(const xenos::xe_gpu_texture_fetch_t& f, uint32_t width, uint32_t height,
                                               uint32_t layers, uint32_t max_level) {
  return texture_util::GetGuestTextureLayout(f.dimension, f.pitch, width, height, layers, f.tiled, f.format,
                                             f.packed_mips, true, max_level);
}

static uint32_t Word(const std::vector<uint8_t>& level, size_t index) {
  uint32_t value;
  std::memcpy(&value, level.data() + index * 4, 4);
  return value;
}

int main() {
  std::vector<uint8_t> memory(1 << 22, 0);
  uint8_t* base = memory.data();
  {
    auto f = Fetch(xenos::TextureFormat::k_8_8_8_8, 32, 32, true, 0x1000, xenos::DataDimension::k2DOrStacked);
    auto layout = Layout(f, 32, 32, 1, 0);
    const uint32_t pitch_blocks = layout.base.row_pitch_bytes / 4;
    for (uint32_t y = 0; y < 32; ++y) {
      for (uint32_t x = 0; x < 32; ++x) {
        const uint32_t value = y << 16 | x;
        std::memcpy(base + 0x1000 + texture_util::GetTiledOffset2D(x, y, pitch_blocks, 2), &value, 4);
      }
    }
    auto loaded = LoadTexture(f, base);
    Check(loaded && loaded->width == 32 && loaded->height == 32 && loaded->mips == 1 && loaded->layers == 1,
          "tiled_8888_dimensions");
    Check(loaded && Word(loaded->levels[0], 7 * 32 + 5) == (7u << 16 | 5) && Word(loaded->levels[0], 32 * 32 - 1) ==
                                                                                   (31u << 16 | 31),
          "tiled_8888_untiles_by_tiled_offset");
    f.endianness = xenos::Endian::k8in32;
    auto swapped = LoadTexture(f, base);
    Check(swapped && Word(swapped->levels[0], 7 * 32 + 5) == __builtin_bswap32(7u << 16 | 5), "endian_8in32_swapped");
  }
  {
    auto f = Fetch(xenos::TextureFormat::k_8_8_8_8, 16, 4, false, 0x10000, xenos::DataDimension::k2DOrStacked);
    f.pitch = 2;
    auto layout = Layout(f, 16, 4, 1, 0);
    for (uint32_t y = 0; y < 4; ++y) {
      for (uint32_t x = 0; x < 16; ++x) {
        const uint32_t value = 0xA0000000 | y << 8 | x;
        std::memcpy(base + 0x10000 + y * layout.base.row_pitch_bytes + x * 4, &value, 4);
      }
    }
    auto loaded = LoadTexture(f, base);
    Check(layout.base.row_pitch_bytes == 256, "linear_pitch_is_256_bytes");
    Check(loaded && Word(loaded->levels[0], 3 * 16 + 9) == (0xA0000000u | 3 << 8 | 9),
          "linear_rows_use_256_byte_pitch");
  }
  {
    auto f = Fetch(xenos::TextureFormat::k_DXT1, 64, 64, true, 0x20000, xenos::DataDimension::k2DOrStacked);
    auto layout = Layout(f, 64, 64, 1, 0);
    const uint32_t pitch_blocks = layout.base.row_pitch_bytes / 8;
    for (uint32_t y = 0; y < 16; ++y) {
      for (uint32_t x = 0; x < 16; ++x) {
        const uint64_t value = uint64_t(y) << 32 | x;
        std::memcpy(base + 0x20000 + texture_util::GetTiledOffset2D(x, y, pitch_blocks, 3), &value, 8);
      }
    }
    auto loaded = LoadTexture(f, base);
    uint64_t block = 0;
    if (loaded) {
      std::memcpy(&block, loaded->levels[0].data() + (5 * 16 + 3) * 8, 8);
    }
    Check(loaded && loaded->host.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && loaded->levels[0].size() == 16 * 16 * 8 &&
              block == (uint64_t(5) << 32 | 3),
          "dxt1_blocks_untiled");
  }
  {
    auto f = Fetch(xenos::TextureFormat::k_CTX1, 32, 32, false, 0x40000, xenos::DataDimension::k2DOrStacked);
    auto layout = Layout(f, 32, 32, 1, 0);
    const uint8_t ctx1[8] = {100, 200, 0, 0, 0, 0, 0, 0};
    for (uint32_t y = 0; y < 8; ++y) {
      for (uint32_t x = 0; x < 8; ++x) {
        std::memcpy(base + 0x40000 + y * layout.base.row_pitch_bytes + x * 8, ctx1, 8);
      }
    }
    auto loaded = LoadTexture(f, base);
    Check(loaded && loaded->host.format == VK_FORMAT_R8G8_UNORM && loaded->levels[0].size() == 32 * 32 * 2 &&
              loaded->levels[0][(9 * 32 + 13) * 2] == 200 && loaded->levels[0][(9 * 32 + 13) * 2 + 1] == 100,
          "ctx1_converted_to_r8g8");
  }
  {
    auto f = Fetch(xenos::TextureFormat::k_8_8_8_8, 32, 32, true, 0x80000, xenos::DataDimension::kCube);
    auto layout = Layout(f, 32, 32, 6, 0);
    const uint32_t pitch_blocks = layout.base.row_pitch_bytes / 4;
    for (uint32_t face = 0; face < 6; ++face) {
      const uint32_t value = 0xF0 + face;
      std::memcpy(base + 0x80000 + face * layout.base.array_slice_stride_bytes +
                      texture_util::GetTiledOffset2D(2, 3, pitch_blocks, 2),
                  &value, 4);
    }
    auto loaded = LoadTexture(f, base);
    Check(loaded && loaded->cube && loaded->layers == 6 && Word(loaded->levels[0], 4 * 32 * 32 + 3 * 32 + 2) == 0xF4,
          "cube_has_six_layers");
  }
  {
    auto f = Fetch(xenos::TextureFormat::k_24_8, 32, 32, true, 0x100000, xenos::DataDimension::k2DOrStacked);
    auto layout = Layout(f, 32, 32, 1, 0);
    const uint32_t pitch_blocks = layout.base.row_pitch_bytes / 4;
    const uint32_t one = 0xFFFFFF00;
    const uint32_t half = 0x80000000;
    std::memcpy(base + 0x100000 + texture_util::GetTiledOffset2D(0, 0, pitch_blocks, 2), &one, 4);
    std::memcpy(base + 0x100000 + texture_util::GetTiledOffset2D(1, 0, pitch_blocks, 2), &half, 4);
    auto loaded = LoadTexture(f, base);
    float depth[2] = {};
    if (loaded) {
      std::memcpy(depth, loaded->levels[0].data(), 8);
    }
    Check(loaded && loaded->host.format == VK_FORMAT_R32_SFLOAT && depth[0] == 1.0f && depth[1] > 0.4999f &&
              depth[1] < 0.5001f,
          "k24_8_becomes_float_depth");
  }
  {
    auto f = Fetch(xenos::TextureFormat::k_8_8_8_8, 64, 64, true, 0x200000, xenos::DataDimension::k2DOrStacked);
    f.mip_address = 0x300000 >> 12;
    f.mip_max_level = 1;
    auto layout = Layout(f, 64, 64, 1, 1);
    const uint32_t pitch_blocks = layout.mips[1].row_pitch_bytes / 4;
    const uint32_t value = 0x1234ABCD;
    std::memcpy(base + 0x300000 + layout.mip_offsets_bytes[1] + texture_util::GetTiledOffset2D(6, 9, pitch_blocks, 2),
                &value, 4);
    auto loaded = LoadTexture(f, base);
    Check(loaded && loaded->mips == 2 && loaded->levels[1].size() == 32 * 32 * 4 &&
              Word(loaded->levels[1], 9 * 32 + 6) == 0x1234ABCD,
          "mip_level_loaded_from_mip_address");
  }
  {
    Check(!LoadTexture(Fetch(xenos::TextureFormat::k_1_5_5_5, 32, 32, true, 0x1000,
                             xenos::DataDimension::k2DOrStacked),
                       base),
          "unknown_format_not_loaded");
    auto footprint = FootprintOf(Fetch(xenos::TextureFormat::k_8_8_8_8, 32, 32, true, 0x1000,
                                       xenos::DataDimension::k2DOrStacked));
    Check(footprint.base_address == 0x1000 && footprint.base_size == 32 * 32 * 4 && footprint.mip_size == 0,
          "footprint_of_base_level");
  }
  {
    Check(NeedsRehash(4096, 10, 10, true) && NeedsRehash(65536, 10, 10, true), "small_texture_rehashed_every_use");
    Check(!NeedsRehash(1 << 20, 10, 10, false) && !NeedsRehash(1 << 20, 13, 10, false) &&
              NeedsRehash(1 << 20, 14, 10, false) && !NeedsRehash(1 << 20, 14, 10, true),
          "large_texture_rehashed_every_fourth_frame");
  }
  {
    Check(ResolveImageMatches(VK_FORMAT_R8G8B8A8_UNORM, 1280, 720, VK_FORMAT_R8G8B8A8_UNORM, 1280, 720) &&
              !ResolveImageMatches(VK_FORMAT_R8G8B8A8_UNORM, 1280, 720, VK_FORMAT_R32_SFLOAT, 1280, 720) &&
              !ResolveImageMatches(VK_FORMAT_R8G8B8A8_UNORM, 1280, 720, VK_FORMAT_R8G8B8A8_UNORM, 640, 360),
          "resolve_image_used_only_on_match");
  }
  return failures ? 1 : 0;
}
