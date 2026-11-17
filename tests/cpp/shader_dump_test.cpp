#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "shader_dump.h"

using nier::native::ShaderDump;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static size_t Count(const std::filesystem::path& dir, const char* prefix) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    const auto name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".ucode") ++count;
  }
  return count;
}

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "nier_shader_dump_test";
  std::filesystem::remove_all(dir);
  ShaderDump dump(dir);
  const std::vector<uint32_t> a = {1, 2, 3, 4};
  const std::vector<uint32_t> b = {5, 6, 7, 8};
  Check(dump.Record(1, a.data(), uint32_t(a.size())), "first record is new");
  Check(!dump.Record(1, a.data(), uint32_t(a.size())), "shader_dump_writes_once");
  Check(dump.Record(0, b.data(), uint32_t(b.size())), "other shader is new");
  Check(dump.unique() == 2, "unique count");
  Check(Count(dir, "ps_") == 1 && Count(dir, "vs_") == 1, "shader_dump_names_by_stage");
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().string().rfind("ps_", 0) == 0) {
      Check(std::filesystem::file_size(entry.path()) == 16, "dump keeps the microcode bytes");
    }
  }
  std::filesystem::remove_all(dir);
  return failures ? 1 : 0;
}
