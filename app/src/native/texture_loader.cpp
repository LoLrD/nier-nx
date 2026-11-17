#include "texture_loader.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>

namespace nier::native {

using namespace rex::graphics;
using Convert = HostTextureFormat::Convert;

namespace {

constexpr uint32_t kSmallTextureBytes = 65536;
constexpr uint64_t kRehashFrames = 4;

struct Storage {
  std::vector<uint8_t> data;
  uint32_t pitch_blocks;
  uint32_t rows;
};

struct Source {
  xenos::xe_gpu_texture_fetch_t fetch;
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t mips;
  uint32_t base_address;
  uint32_t mip_address;
  texture_util::TextureGuestLayout layout;
};

std::optional<Source> Describe(const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked && fetch.dimension != xenos::DataDimension::kCube) {
    return std::nullopt;
  }
  uint32_t width_minus_1, height_minus_1, layers_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &layers_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  if (!base_page) {
    return std::nullopt;
  }
  Source source;
  source.fetch = fetch;
  source.width = width_minus_1 + 1;
  source.height = height_minus_1 + 1;
  source.layers = layers_minus_1 + 1;
  source.mips = mip_page ? mip_max + 1 : 1;
  source.base_address = base_page << 12;
  source.mip_address = mip_page << 12;
  source.layout = texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, source.width, source.height,
                                                      source.layers, fetch.tiled, fetch.format, fetch.packed_mips,
                                                      true, source.mips - 1);
  return source;
}

Storage Untile(const uint8_t* src, const texture_util::TextureGuestLayout::Level& level, const Source& source,
               uint32_t bytes_per_block, bool raw) {
  Storage storage{{}, level.x_extent_blocks, level.y_extent_blocks};
  storage.data.resize(size_t(storage.pitch_blocks) * storage.rows * source.layers * bytes_per_block);
  const uint32_t guest_pitch_blocks = level.row_pitch_bytes / bytes_per_block;
  const uint32_t bytes_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const xenos::Endian endian = raw ? xenos::Endian::kNone : source.fetch.endianness;
  for (uint32_t layer = 0; layer < source.layers; ++layer) {
    const uint8_t* layer_src = src + size_t(layer) * level.array_slice_stride_bytes;
    for (uint32_t y = 0; y < storage.rows; ++y) {
      uint8_t* dst = storage.data.data() + (size_t(layer) * storage.rows + y) * storage.pitch_blocks * bytes_per_block;
      if (!source.fetch.tiled) {
        texture_conversion::CopySwapBlock(endian, dst, layer_src + size_t(y) * level.row_pitch_bytes,
                                          size_t(storage.pitch_blocks) * bytes_per_block);
        continue;
      }
      for (uint32_t x = 0; x < storage.pitch_blocks; ++x) {
        const int32_t offset = texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), guest_pitch_blocks, bytes_log2);
        texture_conversion::CopySwapBlock(endian, dst + size_t(x) * bytes_per_block, layer_src + offset,
                                          bytes_per_block);
      }
    }
  }
  return storage;
}

void ExtractLevel(const Storage& storage, const Source& source, const HostTextureFormat& host, uint32_t level,
                  uint32_t offset_x, uint32_t offset_y, std::vector<uint8_t>& out) {
  const uint32_t width = std::max(source.width >> level, 1u);
  const uint32_t height = std::max(source.height >> level, 1u);
  const uint32_t blocks_x = (width + host.block_width - 1) / host.block_width;
  const uint32_t blocks_y = (height + host.block_height - 1) / host.block_height;
  const uint32_t bpb = host.bytes_per_block;
  auto block = [&](uint32_t layer, uint32_t x, uint32_t y) {
    return storage.data.data() +
           ((size_t(layer) * storage.rows + offset_y + y) * storage.pitch_blocks + offset_x + x) * bpb;
  };
  if (host.convert == Convert::kCtx1ToR8G8) {
    const uint32_t padded_width = blocks_x * 4;
    std::vector<uint8_t> padded(size_t(padded_width) * blocks_y * 4 * 2);
    out.resize(size_t(width) * height * 2 * source.layers);
    for (uint32_t layer = 0; layer < source.layers; ++layer) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          texture_conversion::ConvertTexelCTX1ToR8G8(source.fetch.endianness,
                                                     padded.data() + (size_t(y) * 4 * padded_width + x * 4) * 2,
                                                     block(layer, x, y), padded_width * 2);
        }
      }
      for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(out.data() + (size_t(layer) * height + row) * width * 2,
                    padded.data() + size_t(row) * padded_width * 2, size_t(width) * 2);
      }
    }
    return;
  }
  out.resize(size_t(blocks_x) * blocks_y * bpb * source.layers);
  uint8_t* dst = out.data();
  for (uint32_t layer = 0; layer < source.layers; ++layer) {
    for (uint32_t y = 0; y < blocks_y; ++y) {
      if (host.convert == Convert::kDepthUnormToFloat) {
        for (uint32_t x = 0; x < blocks_x; ++x, dst += 4) {
          uint32_t value;
          std::memcpy(&value, block(layer, x, y), 4);
          const float depth = float(value >> 8) * (1.0f / 16777215.0f);
          std::memcpy(dst, &depth, 4);
        }
        continue;
      }
      std::memcpy(dst, block(layer, 0, y), size_t(blocks_x) * bpb);
      dst += size_t(blocks_x) * bpb;
    }
  }
}

}  // namespace

std::optional<HostTextureFormat> HostFormatFor(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8:
      return HostTextureFormat{VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, 1, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
                               Convert::kNone};
    case F::k_8_8:
      return HostTextureFormat{VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, 1, 1, 2, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
                               Convert::kNone};
    case F::k_8_8_8_8:
      return HostTextureFormat{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, 1, 1, 4,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, Convert::kNone};
    case F::k_DXT1:
      return HostTextureFormat{VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, Convert::kNone};
    case F::k_DXT2_3:
      return HostTextureFormat{VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, Convert::kNone};
    case F::k_DXT4_5:
      return HostTextureFormat{VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, Convert::kNone};
    case F::k_24_8:
      return HostTextureFormat{VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, 1, 1, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
                               Convert::kDepthUnormToFloat};
    case F::k_16_16_16_16_EXPAND:
    case F::k_16_16_16_16_FLOAT:
      return HostTextureFormat{VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, Convert::kNone};
    case F::k_16_16_FLOAT:
      return HostTextureFormat{VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, 1, 1, 4,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, Convert::kNone};
    case F::k_32_FLOAT:
      return HostTextureFormat{VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, 1, 1, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
                               Convert::kNone};
    case F::k_32_32_32_32_FLOAT:
      return HostTextureFormat{VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 16,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, Convert::kNone};
    case F::k_CTX1:
      return HostTextureFormat{VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 4, 4, 8,
                               xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, Convert::kCtx1ToR8G8};
    default:
      return std::nullopt;
  }
}

TextureFootprint FootprintOf(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const auto source = Describe(fetch);
  if (!source) {
    return {};
  }
  return {source->base_address, source->layout.base.level_data_extent_bytes, source->mip_address,
          source->mips > 1 ? source->layout.mips_total_extent_bytes : 0};
}

std::optional<LoadedTexture> LoadTexture(const xenos::xe_gpu_texture_fetch_t& fetch, const uint8_t* physical_base) {
  const auto host = HostFormatFor(fetch.format);
  const auto source = Describe(fetch);
  if (!host || !source) {
    return std::nullopt;
  }
  const bool raw = host->convert == Convert::kCtx1ToR8G8;
  const uint32_t packed = source->layout.packed_level;
  LoadedTexture loaded{*host, source->width, source->height, source->layers, source->mips,
                       fetch.dimension == xenos::DataDimension::kCube, {}};
  loaded.levels.resize(source->mips);
  const Storage base = Untile(physical_base + source->base_address, source->layout.base, *source,
                              host->bytes_per_block, raw);
  std::vector<std::optional<Storage>> mip_storage(source->mips);
  for (uint32_t level = 0; level < source->mips; ++level) {
    uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
    if (level >= packed) {
      texture_util::GetPackedMipOffset(source->width, source->height, 1, fetch.format, level, offset_x, offset_y,
                                       offset_z);
    }
    if (!level) {
      ExtractLevel(base, *source, *host, 0, offset_x, offset_y, loaded.levels[0]);
      continue;
    }
    const uint32_t stored = std::min(level, packed);
    if (!mip_storage[stored]) {
      mip_storage[stored] =
          Untile(physical_base + source->mip_address + source->layout.mip_offsets_bytes[stored],
                 source->layout.mips[stored], *source, host->bytes_per_block, raw);
    }
    ExtractLevel(*mip_storage[stored], *source, *host, level, offset_x, offset_y, loaded.levels[level]);
  }
  return loaded;
}

bool NeedsRehash(uint32_t bytes, uint64_t frame, uint64_t last_checked_frame, bool used_this_frame) {
  if (bytes <= kSmallTextureBytes) {
    return true;
  }
  return !used_this_frame && frame >= last_checked_frame + kRehashFrames;
}

bool ResolveImageMatches(VkFormat image_format, uint32_t image_width, uint32_t image_height, VkFormat wanted_format,
                         uint32_t width, uint32_t height) {
  return image_format == wanted_format && image_width == width && image_height == height;
}

}  // namespace nier::native
