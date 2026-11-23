#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace nier {

struct CpkFile {
  std::string path;
  uint64_t offset;
  uint64_t size;
};

using CpkReader = std::function<bool(uint64_t offset, std::span<uint8_t> out)>;

std::vector<CpkFile> ReadCpkToc(const CpkReader& read);

}  // namespace nier
