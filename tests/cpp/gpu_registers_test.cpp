#include <cstdint>
#include <cstdio>

#include <rex/graphics/registers.h>

#include "gpu_registers.h"

using nier::native::GpuRegisters;
using namespace rex::graphics;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  {
    GpuRegisters regs;
    regs.Set(XE_GPU_REG_SCRATCH_UMSK, 0b1);
    regs.Set(XE_GPU_REG_SCRATCH_ADDR, 0x1000);
    const auto store = regs.Set(XE_GPU_REG_SCRATCH_REG0, 7);
    Check(store && store->address == 0x1000 && store->value == 7, "scratch_writeback_when_unmasked");
    Check(regs.Get(XE_GPU_REG_SCRATCH_REG0) == 7, "scratch_writeback_when_unmasked mirror");
  }
  {
    GpuRegisters regs;
    regs.Set(XE_GPU_REG_SCRATCH_UMSK, 0b10);
    regs.Set(XE_GPU_REG_SCRATCH_ADDR, 0x1000);
    Check(!regs.Set(XE_GPU_REG_SCRATCH_REG0, 7), "scratch_masked_no_writeback");
  }
  {
    GpuRegisters regs;
    regs.Set(XE_GPU_REG_SCRATCH_UMSK, 0xFF);
    regs.Set(XE_GPU_REG_SCRATCH_ADDR, 0x2000);
    const auto store = regs.Set(XE_GPU_REG_SCRATCH_REG3, 9);
    Check(store && store->address == 0x2000 + 12 && store->value == 9, "scratch_reg3_offset");
  }
  {
    GpuRegisters regs;
    Check(regs.ReadMmio(0x0F00, 1280, 720) == 0x08100748, "mmio edram timing");
    Check(regs.ReadMmio(0x0F01, 1280, 720) == 0x0000200E, "mmio bc control");
    Check(regs.ReadMmio(0x194C, 1280, 720) == 720, "mmio v counter");
    Check(regs.ReadMmio(0x1951, 1280, 720) == 1, "mmio vblank status");
    Check(regs.ReadMmio(0x1961, 1280, 720) == 0x050002D0, "mmio viewport size");
    Check(regs.ReadMmio(0x194C, 8000, 8000) == 0x0FFF, "mmio clamps to 12 bits");
    regs.Set(0x2000, 0xABCD);
    Check(regs.ReadMmio(0x2000, 1280, 720) == 0xABCD, "mmio falls back to the mirror");
  }
  {
    GpuRegisters regs;
    Check(!regs.Set(0x5003, 1), "out_of_range_ignored set");
    Check(regs.Get(0x5003) == 0 && regs.ReadMmio(0x5003, 1280, 720) == 0, "out_of_range_ignored read");
  }
  {
    GpuRegisters regs;
    regs.Set(XE_GPU_REG_SQ_PROGRAM_CNTL, 0x10000203);
    Check(regs.file().Get<reg::SQ_PROGRAM_CNTL>().value == 0x10000203, "register_file_view");
  }
  return failures ? 1 : 0;
}
