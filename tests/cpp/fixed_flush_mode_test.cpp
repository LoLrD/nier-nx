#define REX_PPC_FIXED_FLUSH_MODE 1

#include <cstdint>
#include <cstdio>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wasm-operand-widths"
#include <rex/ppc/context.h>
#pragma clang diagnostic pop

int main() {
  rex::ppc::FPSCRRegister fpscr{};
  fpscr.InitHost();
  const uint32_t before = fpscr.getcsr();
  fpscr.enableFlushMode();
  const bool kept_on_enable = fpscr.getcsr() == before;
  fpscr.disableFlushMode();
  const bool kept_on_disable = fpscr.getcsr() == before;
  fpscr.enableFlushModeUnconditional();
  const bool kept_on_unconditional_enable = fpscr.getcsr() == before;
  fpscr.disableFlushModeUnconditional();
  const bool kept_on_unconditional_disable = fpscr.getcsr() == before;
  if (!kept_on_enable || !kept_on_disable || !kept_on_unconditional_enable || !kept_on_unconditional_disable) {
    std::printf("FAIL: a fixed flush mode still changes the floating point control register\n");
    return 1;
  }
  return 0;
}
