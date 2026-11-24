#include <cmath>
#include <cstdio>
#include <limits>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wasm-operand-widths"
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>
#pragma clang diagnostic pop

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  bool reciprocal_close = true;
  bool rsqrt_close = true;
  for (float x = 1e-6f; x < 1e7f; x *= 1.37f) {
    alignas(16) float out[4];
    simde_mm_store_ps(out, rex::ppc::simde_mm_vrefp(simde_mm_set_ps(-x, x, 2 * x, 0.5f * x)));
    const float expected[4] = {1.0f / (0.5f * x), 1.0f / (2 * x), 1.0f / x, -1.0f / x};
    for (int i = 0; i < 4; ++i) {
      reciprocal_close &= std::fabs(out[i] - expected[i]) <= std::fabs(expected[i]) / 4096.0f;
    }
    simde_mm_store_ps(out, rex::ppc::simde_mm_vrsqrtefp(simde_mm_set1_ps(x)));
    rsqrt_close &= std::fabs(out[0] - 1.0f / std::sqrt(x)) <= (1.0f / std::sqrt(x)) / 4096.0f;
  }
  Check(reciprocal_close, "reciprocal_estimate_within_ppc_precision");
  Check(rsqrt_close, "rsqrt_estimate_within_ppc_precision");
  alignas(16) float edge[4];
  simde_mm_store_ps(edge, rex::ppc::simde_mm_vrefp(simde_mm_set_ps(0.0f, -0.0f, 0.0f, -0.0f)));
  Check(std::isinf(edge[0]) && edge[0] < 0 && std::isinf(edge[1]) && edge[1] > 0, "reciprocal_of_zero_is_signed_infinity");
  simde_mm_store_ps(edge, rex::ppc::simde_mm_vrsqrtefp(simde_mm_set_ps(-1.0f, 0.0f, 4.0f, 1.0f)));
  Check(std::fabs(edge[0] - 1.0f) < 1e-4f && std::fabs(edge[1] - 0.5f) < 1e-4f && std::isinf(edge[2]) && std::isnan(edge[3]),
        "rsqrt_edges");
  return failures ? 1 : 0;
}
