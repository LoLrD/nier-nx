#include <cstdio>

#include "host_vertex_type.h"

using namespace rex::graphics;
using nier::native::HostVertexShaderTypeFor;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  Check(HostVertexShaderTypeFor(xenos::PrimitiveType::kRectangleList) ==
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip,
        "rectangle list expands in the vertex shader");
  Check(HostVertexShaderTypeFor(xenos::PrimitiveType::kPointList) ==
            Shader::HostVertexShaderType::kPointListAsTriangleStrip,
        "point list expands in the vertex shader");
  Check(HostVertexShaderTypeFor(xenos::PrimitiveType::kTriangleList) == Shader::HostVertexShaderType::kVertex,
        "triangle list is a plain vertex shader");
  return failures ? 1 : 0;
}
