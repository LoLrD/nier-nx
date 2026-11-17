#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace nier::native {

struct MemoryStore {
  uint32_t address;
  uint32_t value;
};

class GpuRegisters {
 public:
  static constexpr uint32_t kCount = 0x5003;

  uint32_t Get(uint32_t index) const;
  std::optional<MemoryStore> Set(uint32_t index, uint32_t value);
  uint32_t ReadMmio(uint32_t index, uint32_t display_width, uint32_t display_height) const;

 private:
  std::array<uint32_t, kCount> values_{};
};

}  // namespace nier::native
