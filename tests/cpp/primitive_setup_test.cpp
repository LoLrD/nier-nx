#include <cstdint>
#include <cstdio>
#include <memory>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

#include "primitive_setup.h"

using namespace rex::graphics;
using namespace nier::native;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static void Draw(RegisterFile& r, xenos::PrimitiveType prim, xenos::SourceSelect source, uint32_t count) {
  reg::VGT_DRAW_INITIATOR initiator{};
  initiator.prim_type = prim;
  initiator.source_select = source;
  initiator.index_size = xenos::IndexFormat::kInt16;
  initiator.num_indices = count;
  r[XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator.value;
}

int main() {
  auto regs = std::make_unique<RegisterFile>();
  RegisterFile& r = *regs;
  {
    Draw(r, xenos::PrimitiveType::kTriangleStrip, xenos::SourceSelect::kDMA, 300);
    r[XE_GPU_REG_VGT_DMA_BASE] = 0x01234567;
    reg::VGT_DMA_SIZE size{};
    size.num_words = 1000;
    size.swap_mode = xenos::Endian::k8in16;
    r[XE_GPU_REG_VGT_DMA_SIZE] = size.value;
    reg::PA_SU_SC_MODE_CNTL mode{};
    mode.multi_prim_ib_ena = 1;
    r[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
    r[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] = 0xFFFF;
    HostDraw d = SetupPrimitive(r);
    Check(d.supported && d.index_source == IndexSource::kGuestDma && d.topology == HostTopology::kTriangleStrip &&
              d.vs_type == Shader::HostVertexShaderType::kVertex,
          "indexed_strip_uses_guest_dma");
    Check(d.guest_index_address == 0x01234566 && d.guest_index_bytes == 600 && !d.index_32bit && d.host_count == 300,
          "indexed_strip_index_range");
    Check(d.index_endian == xenos::Endian::k8in16 && d.primitive_restart, "indexed_strip_endian_and_restart");
    r[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] = 0x1234;
    Check(!SetupPrimitive(r).supported, "indexed_strip_with_other_reset_unsupported");
    r[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0;
    r[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] = 0;
  }
  {
    Draw(r, xenos::PrimitiveType::kTriangleList, xenos::SourceSelect::kAutoIndex, 36);
    HostDraw d = SetupPrimitive(r);
    Check(d.supported && d.index_source == IndexSource::kNone && d.topology == HostTopology::kTriangleList &&
              d.host_count == 36 && !d.primitive_restart,
          "auto_triangle_list_has_no_indices");
  }
  {
    Draw(r, xenos::PrimitiveType::kRectangleList, xenos::SourceSelect::kAutoIndex, 6);
    HostDraw d = SetupPrimitive(r);
    Check(d.supported && d.index_source == IndexSource::kBuiltinRectangles &&
              d.vs_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
              d.topology == HostTopology::kTriangleStrip && d.primitive_restart && d.host_count == 9 && d.index_32bit,
          "auto_rectangles_expand_in_vertex_shader");
  }
  {
    Draw(r, xenos::PrimitiveType::kQuadList, xenos::SourceSelect::kAutoIndex, 8);
    HostDraw d = SetupPrimitive(r);
    Check(d.supported && d.index_source == IndexSource::kBuiltinQuads && d.topology == HostTopology::kTriangleList &&
              d.vs_type == Shader::HostVertexShaderType::kVertex && d.host_count == 12,
          "auto_quads_become_triangle_list");
  }
  {
    Draw(r, xenos::PrimitiveType::kPointList, xenos::SourceSelect::kAutoIndex, 4);
    Check(!SetupPrimitive(r).supported, "point_list_unsupported");
    Draw(r, xenos::PrimitiveType::kQuadList, xenos::SourceSelect::kDMA, 4);
    Check(!SetupPrimitive(r).supported, "indexed_quads_unsupported");
  }
  {
    auto rectangles = BuildRectangleIndices(2);
    Check(rectangles.size() == 9 && rectangles[0] == 0 && rectangles[3] == 3 && rectangles[4] == 0xFFFFFFFF &&
              rectangles[5] == 4 && rectangles[8] == 7,
          "rectangle_indices_layout");
    auto quads = BuildQuadIndices(2);
    Check(quads.size() == 12 && quads[0] == 0 && quads[1] == 1 && quads[2] == 2 && quads[3] == 0 && quads[4] == 2 &&
              quads[5] == 3 && quads[6] == 4 && quads[11] == 7,
          "quad_indices_layout");
  }
  return failures ? 1 : 0;
}
