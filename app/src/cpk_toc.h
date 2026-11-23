#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace nier {

struct CpkCell {
  uint64_t at = 0;
  uint8_t width = 0;
};

struct CpkFile {
  std::string path;
  uint64_t offset;
  uint64_t size;
  uint64_t base = 0;
  CpkCell size_cell;
  CpkCell extract_size_cell;
  CpkCell offset_cell;
};

using CpkReader = std::function<bool(uint64_t offset, std::span<uint8_t> out)>;

std::vector<CpkFile> ReadCpkToc(const CpkReader& read);

}  // namespace nier
