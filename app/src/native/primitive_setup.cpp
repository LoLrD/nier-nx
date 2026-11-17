#include "primitive_setup.h"

#include <algorithm>

#include <rex/graphics/registers.h>

namespace nier::native {

using namespace rex::graphics;

namespace {

HostDraw Unsupported() {
  HostDraw draw{};
  draw.supported = false;
  return draw;
}

bool IsStripType(xenos::PrimitiveType type) {
  switch (type) {
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kLineLoop:
    case xenos::PrimitiveType::kQuadStrip:
    case xenos::PrimitiveType::kPolygon:
    case xenos::PrimitiveType::k2DLineStrip:
    case xenos::PrimitiveType::k2DTriStrip:
      return true;
    default:
      return false;
  }
}

HostDraw SetupGuestDma(const RegisterFile& regs, xenos::PrimitiveType type, uint32_t count) {
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  const auto dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  HostDraw draw{};
  draw.supported = true;
  draw.topology = type == xenos::PrimitiveType::kTriangleList ? HostTopology::kTriangleList
                                                              : HostTopology::kTriangleStrip;
  draw.vs_type = Shader::HostVertexShaderType::kVertex;
  draw.index_source = IndexSource::kGuestDma;
  draw.index_32bit = initiator.index_size == xenos::IndexFormat::kInt32;
  xenos::Endian endian = dma_size.swap_mode;
  if (!draw.index_32bit && endian != xenos::Endian::kNone && endian != xenos::Endian::k8in16) {
    endian = endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  draw.index_endian = endian;
  const uint32_t size_log2 = draw.index_32bit ? 2 : 1;
  draw.guest_index_address = regs[XE_GPU_REG_VGT_DMA_BASE] & ~((1u << size_log2) - 1);
  draw.guest_index_bytes = count << size_log2;
  draw.host_count = count;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena && IsStripType(type)) {
    const uint32_t reset = xenos::GpuSwap(regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx, endian);
    const bool enabled = draw.index_32bit || reset <= 0xFFFF;
    if (enabled && reset != (draw.index_32bit ? 0xFFFFFFFFu : 0xFFFFu)) {
      return Unsupported();
    }
    draw.primitive_restart = enabled;
  }
  return draw;
}

}  // namespace

HostDraw SetupPrimitive(const RegisterFile& regs) {
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select == xenos::VGTOutputPath::kTessellationEnable) {
    return Unsupported();
  }
  const xenos::PrimitiveType type = initiator.prim_type;
  const bool auto_index = initiator.source_select == xenos::SourceSelect::kAutoIndex;
  if (!auto_index && initiator.source_select != xenos::SourceSelect::kDMA) {
    return Unsupported();
  }
  uint32_t count = initiator.num_indices;
  if (!auto_index) {
    count = std::min(count, uint32_t(regs.Get<reg::VGT_DMA_SIZE>().num_words));
  }
  HostDraw draw{};
  draw.supported = true;
  draw.vs_type = Shader::HostVertexShaderType::kVertex;
  draw.index_endian = xenos::Endian::kNone;
  switch (type) {
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleStrip:
      if (!auto_index) {
        return SetupGuestDma(regs, type, count);
      }
      draw.topology =
          type == xenos::PrimitiveType::kTriangleList ? HostTopology::kTriangleList : HostTopology::kTriangleStrip;
      draw.index_source = IndexSource::kNone;
      draw.host_count = count;
      return draw;
    case xenos::PrimitiveType::kRectangleList: {
      const uint32_t rectangles = count / 3;
      if (!auto_index || rectangles > kMaxBuiltinRectangles) {
        return Unsupported();
      }
      draw.topology = HostTopology::kTriangleStrip;
      draw.primitive_restart = true;
      draw.vs_type = Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
      draw.index_source = IndexSource::kBuiltinRectangles;
      draw.index_32bit = true;
      draw.host_count = 4 * rectangles + (std::max(rectangles, 1u) - 1);
      return draw;
    }
    case xenos::PrimitiveType::kQuadList: {
      const uint32_t quads = count / 4;
      if (!auto_index || quads > kMaxBuiltinQuads) {
        return Unsupported();
      }
      draw.topology = HostTopology::kTriangleList;
      draw.index_source = IndexSource::kBuiltinQuads;
      draw.index_32bit = true;
      draw.host_count = quads * 6;
      return draw;
    }
    default:
      return Unsupported();
  }
}

std::vector<uint32_t> BuildRectangleIndices(uint32_t max_rectangles) {
  std::vector<uint32_t> indices;
  indices.reserve(size_t(max_rectangles) * 5);
  for (uint32_t i = 0; i < max_rectangles; ++i) {
    if (i) {
      indices.push_back(0xFFFFFFFF);
    }
    for (uint32_t j = 0; j < 4; ++j) {
      indices.push_back(i << 2 | j);
    }
  }
  return indices;
}

std::vector<uint32_t> BuildQuadIndices(uint32_t max_quads) {
  std::vector<uint32_t> indices;
  indices.reserve(size_t(max_quads) * 6);
  for (uint32_t i = 0; i < max_quads; ++i) {
    const uint32_t first = i * 4;
    indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
  }
  return indices;
}

}  // namespace nier::native
