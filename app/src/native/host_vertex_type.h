#pragma once

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

namespace nier::native {

inline rex::graphics::Shader::HostVertexShaderType HostVertexShaderTypeFor(
    rex::graphics::xenos::PrimitiveType type) {
  using HostType = rex::graphics::Shader::HostVertexShaderType;
  switch (type) {
    case rex::graphics::xenos::PrimitiveType::kRectangleList:
      return HostType::kRectangleListAsTriangleStrip;
    case rex::graphics::xenos::PrimitiveType::kPointList:
      return HostType::kPointListAsTriangleStrip;
    default:
      return HostType::kVertex;
  }
}

}  // namespace nier::native
