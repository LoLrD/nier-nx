#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace nier::native {

enum class IndexSource { kNone, kGuestDma, kBuiltinRectangles, kBuiltinQuads };

enum class HostTopology { kTriangleList, kTriangleStrip };

struct HostDraw {
  bool supported;
  HostTopology topology;
  bool primitive_restart;
  rex::graphics::Shader::HostVertexShaderType vs_type;
  IndexSource index_source;
  uint32_t guest_index_address;
  uint32_t guest_index_bytes;
  bool index_32bit;
  rex::graphics::xenos::Endian index_endian;
  uint32_t host_count;
};

constexpr uint32_t kMaxBuiltinRectangles = 0xFFFF / 3;
constexpr uint32_t kMaxBuiltinQuads = 0x10000;

HostDraw SetupPrimitive(const rex::graphics::RegisterFile& regs);
std::vector<uint32_t> BuildRectangleIndices(uint32_t max_rectangles);
std::vector<uint32_t> BuildQuadIndices(uint32_t max_quads);

}  // namespace nier::native
