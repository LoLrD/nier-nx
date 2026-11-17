#include "gpu_registers.h"

#include <algorithm>

#include <rex/graphics/registers.h>

namespace nier::native {

using namespace rex::graphics;

uint32_t GpuRegisters::Get(uint32_t index) const {
  return index < kCount ? file_.values[index] : 0;
}

std::optional<MemoryStore> GpuRegisters::Set(uint32_t index, uint32_t value) {
  if (index >= kCount) {
    return std::nullopt;
  }
  file_.values[index] = value;
  if (index < XE_GPU_REG_SCRATCH_REG0 || index > XE_GPU_REG_SCRATCH_REG7) {
    return std::nullopt;
  }
  const uint32_t scratch = index - XE_GPU_REG_SCRATCH_REG0;
  if (!((1u << scratch) & file_.values[XE_GPU_REG_SCRATCH_UMSK])) {
    return std::nullopt;
  }
  return MemoryStore{file_.values[XE_GPU_REG_SCRATCH_ADDR] + scratch * 4, value};
}

uint32_t GpuRegisters::ReadMmio(uint32_t index, uint32_t display_width, uint32_t display_height) const {
  const uint32_t width = std::min(display_width, uint32_t(0x0FFF));
  const uint32_t height = std::min(display_height, uint32_t(0x0FFF));
  switch (index) {
    case 0x0F00:
      return 0x08100748;
    case 0x0F01:
      return 0x0000200E;
    case 0x194C:
      return height;
    case 0x1951:
      return 1;
    case 0x1961:
      return (width << 16) | height;
    default:
      return Get(index);
  }
}

}  // namespace nier::native
