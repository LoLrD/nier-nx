#include <cstdint>
#include <cstdio>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  Check(texture_util::GetTiledOffset2D(0, 0, 32, 2) == 0, "tiled_origin_is_zero");
  Check(texture_util::GetTiledOffset2D(1, 0, 32, 2) == 4, "tiled_second_texel_is_next_word");
  Check(xenos::GpuSwap(uint32_t(0x11223344), xenos::Endian::k8in32) == 0x44332211, "gpu_swap_8in32");
  return failures ? 1 : 0;
}
