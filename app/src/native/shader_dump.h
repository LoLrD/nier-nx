#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_set>

namespace nier::native {

class ShaderDump {
 public:
  explicit ShaderDump(std::filesystem::path dir);

  bool Record(uint32_t type, const uint32_t* words_big_endian, uint32_t size_words);
  size_t unique() const { return seen_.size(); }

 private:
  std::filesystem::path dir_;
  std::unordered_set<uint64_t> seen_;
};

}  // namespace nier::native
