#include <cstdint>
#include <cstdio>

#include <rex/graphics/pipeline/texture/util.h>

#include "tiling.h"

using namespace rex::graphics;
using nier::native::TiledOffset2D;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  bool same = true;
  for (uint32_t bytes_log2 = 0; bytes_log2 <= 4; ++bytes_log2) {
    for (uint32_t pitch : {32u, 64u, 96u, 160u, 1280u}) {
      for (int32_t y = 0; y < 96; ++y) {
        for (int32_t x = 0; x < int32_t(pitch); ++x) {
          same &= TiledOffset2D(x, y, pitch, bytes_log2) == texture_util::GetTiledOffset2D(x, y, pitch, bytes_log2);
        }
      }
    }
  }
  Check(same, "tiled_offset_matches_sdk");
  return failures ? 1 : 0;
}
