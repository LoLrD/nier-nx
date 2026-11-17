#include "shader_dump.h"

#include <cstdio>
#include <fstream>

namespace nier::native {

namespace {

uint64_t Fnv1a(const uint8_t* bytes, size_t size, uint32_t type) {
  uint64_t hash = 0xCBF29CE484222325ull ^ type;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  }
  return hash;
}

}  // namespace

ShaderDump::ShaderDump(std::filesystem::path dir) : dir_(std::move(dir)) {
  std::filesystem::create_directories(dir_);
}

bool ShaderDump::Record(uint32_t type, uint32_t address, const uint32_t* words_big_endian, uint32_t size_words) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(words_big_endian);
  const size_t size = size_t(size_words) * 4;
  const uint64_t key = Fnv1a(bytes, size, type);
  if (!seen_.insert(key).second) {
    return false;
  }
  char name[32];
  std::snprintf(name, sizeof(name), "%s_%016llx.ucode", type ? "ps" : "vs", static_cast<unsigned long long>(key));
  std::ofstream(dir_ / name, std::ios::binary).write(reinterpret_cast<const char*>(bytes), std::streamsize(size));
  char line[96];
  std::snprintf(line, sizeof(line), "%s\t%u\t%08x\t%u\n", name, type, address, size_words);
  std::ofstream(dir_ / "index.tsv", std::ios::app) << line;
  return true;
}

}  // namespace nier::native
