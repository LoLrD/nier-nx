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
  {
    GpuRegisters regs;
    Check(regs.gamma_ramp()[255] == (0x3FFu << 20 | 0x3FFu << 10 | 0x3FFu) && regs.gamma_ramp()[0] == 0,
          "gamma_ramp_starts_linear");
    const uint64_t version = regs.gamma_version();
    regs.Set(XE_GPU_REG_DC_LUT_WRITE_EN_MASK, 7);
    regs.Set(XE_GPU_REG_DC_LUT_RW_INDEX, 5);
    regs.Set(XE_GPU_REG_DC_LUT_SEQ_COLOR, 0x100 << 6);
    regs.Set(XE_GPU_REG_DC_LUT_SEQ_COLOR, 0x200 << 6);
    regs.Set(XE_GPU_REG_DC_LUT_SEQ_COLOR, 0x300 << 6);
    Check(regs.gamma_ramp()[5] == (0x100u << 20 | 0x200u << 10 | 0x300u), "gamma_ramp_written_red_green_blue");
    Check(regs.Get(XE_GPU_REG_DC_LUT_RW_INDEX) == 6 && regs.gamma_version() != version,
          "gamma_ramp_index_advances");
    regs.Set(XE_GPU_REG_DC_LUT_WRITE_EN_MASK, 4);
    regs.Set(XE_GPU_REG_DC_LUT_SEQ_COLOR, 0x3FF << 6);
    regs.Set(XE_GPU_REG_DC_LUT_SEQ_COLOR, 0x3FF << 6);
    regs.Set(XE_GPU_REG_DC_LUT_SEQ_COLOR, 0x3FF << 6);
    Check((regs.gamma_ramp()[6] >> 20) == 0x3FF && (regs.gamma_ramp()[6] & 0xFFFFF) == ((6u * 0x3FF / 0xFF) << 10 | 6u * 0x3FF / 0xFF),
          "gamma_ramp_write_mask_red_only");
  }
  {
    GpuRegisters regs;
    regs.Set(XE_GPU_REG_DC_LUT_WRITE_EN_MASK, 7);
    regs.Set(XE_GPU_REG_DC_LUT_RW_INDEX, 2);
    regs.Set(XE_GPU_REG_DC_LUT_30_COLOR, 0x00300C03);
    Check(regs.gamma_ramp()[2] == 0x00300C03 && regs.Get(XE_GPU_REG_DC_LUT_RW_INDEX) == 3,
          "gamma_ramp_30_color_writes_entry");
    regs.Set(XE_GPU_REG_DC_LUT_WRITE_EN_MASK, 1);
    regs.Set(XE_GPU_REG_DC_LUT_30_COLOR, 0x3FFFFFFF);
    const uint32_t linear = 3u * 0x3FF / 0xFF;
    Check(regs.gamma_ramp()[3] == (linear << 20 | linear << 10 | 0x3FF), "gamma_ramp_30_color_write_mask_blue_only");
  }
  {
    GpuRegisters regs;
    regs.Set(XE_GPU_REG_DC_LUT_WRITE_EN_MASK, 7);
    regs.Set(XE_GPU_REG_DC_LUT_RW_INDEX, 5);
    const auto before = regs.gamma_ramp();
    regs.Store(XE_GPU_REG_DC_LUT_30_COLOR, 0x3FFFFFFF);
    regs.Store(XE_GPU_REG_SCRATCH_REG0, 0x1234);
    Check(regs.gamma_ramp() == before && regs.Get(XE_GPU_REG_DC_LUT_RW_INDEX) == 5 &&
              regs.Get(XE_GPU_REG_DC_LUT_30_COLOR) == 0x3FFFFFFF && regs.Get(XE_GPU_REG_SCRATCH_REG0) == 0x1234,
          "mmio_store_has_no_side_effects");
    regs.Store(GpuRegisters::kCount, 1);
    Check(regs.Get(GpuRegisters::kCount) == 0, "mmio_store_out_of_range_ignored");
  }
  {
    GpuRegisters regs;
    const uint64_t vs20[4] = {1ull << 20, 0, 0, 0};
    const uint64_t vs_far[4] = {0, 0, 0, 1ull << 5};
    const uint64_t vs_before = regs.FloatGeneration(vs20, false);
    const uint64_t ps_before = regs.FloatGeneration(vs20, true);
    regs.Set(0x4000 + 1024 + 20 * 4 + 2, 0x3F800000);
    Check(regs.FloatGeneration(vs20, true) > ps_before, "pixel_constant_write_changes_its_generation");
    Check(regs.FloatGeneration(vs20, false) == vs_before, "vertex_constants_untouched_by_pixel_write");
    const uint64_t far_before = regs.FloatGeneration(vs_far, false);
    regs.Set(0x4000 + (3 * 64 + 5) * 4, 1);
    Check(regs.FloatGeneration(vs_far, false) > far_before, "high_vertex_constant_write_changes_its_generation");
    Check(regs.FloatGeneration(vs20, false) == vs_before, "other_groups_keep_their_generation");
    const uint64_t loops = regs.ConstantsGeneration(0x4900, 40);
    regs.Store(0x4908, 7);
    Check(regs.ConstantsGeneration(0x4900, 40) > loops, "mmio_store_to_loop_constants_changes_generation");
  }
  {
    GpuRegisters regs;
    const uint32_t words[3] = {__builtin_bswap32(0x3F800000u), __builtin_bswap32(0x40000000u),
                               __builtin_bswap32(0x40400000u)};
    const uint64_t target_before = regs.ConstantsGeneration(0x4000 + 63, 3);
    const uint64_t other_before = regs.ConstantsGeneration(0x4200, 4);
    Check(regs.SetRange(0x4000 + 63, words, 3), "constant_range_is_written_in_bulk");
    Check(regs.Get(0x4000 + 63) == 0x3F800000u && regs.Get(0x4000 + 64) == 0x40000000u &&
              regs.Get(0x4000 + 65) == 0x40400000u,
          "bulk_write_swaps_guest_words");
    Check(regs.ConstantsGeneration(0x4000 + 63, 1) > target_before &&
              regs.ConstantsGeneration(0x4000 + 64, 2) > target_before,
          "bulk_write_touches_every_group_it_covers");
    Check(regs.ConstantsGeneration(0x4200, 4) == other_before, "bulk_write_leaves_other_groups");
    Check(!regs.SetRange(XE_GPU_REG_SCRATCH_REG0, words, 3), "special_registers_are_not_written_in_bulk");
    Check(!regs.SetRange(GpuRegisters::kCount - 2, words, 3), "range_past_the_file_is_not_written_in_bulk");
  }
  return failures ? 1 : 0;
}
