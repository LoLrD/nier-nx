#include <cstdint>
#include <cstdio>
#include <vector>

#include "shader_cache.h"

using nier::native::ShaderCache;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  const std::vector<uint32_t> textures = {0x11, 0x22};
  const std::vector<uint32_t> samplers = {0x33};
  const std::vector<uint8_t> spirv = {1, 2, 3, 4, 5, 6, 7, 8};
  std::vector<uint8_t> file;
  {
    ShaderCache cache(42);
    cache.Load({});
    Check(!cache.Has(7, 9, 1), "empty_cache_has_nothing");
    file = cache.Header();
    const auto record = cache.Add(7, 9, 1, textures, samplers, spirv);
    file.insert(file.end(), record.begin(), record.end());
    Check(cache.Add(7, 9, 1, textures, samplers, spirv).empty(), "a_translation_is_recorded_once");
  }
  {
    ShaderCache cache(42);
    cache.Load(file);
    Check(cache.Has(7, 9, 1), "translation_survives_a_reload");
    Check(!cache.Has(7, 9, 2) && !cache.Has(7, 10, 1) && !cache.Has(8, 9, 1), "other_keys_are_misses");
    std::vector<uint32_t> loaded_textures, loaded_samplers;
    std::vector<uint8_t> loaded_spirv;
    Check(cache.Take(7, 9, 1, loaded_textures, loaded_samplers, loaded_spirv), "translation_can_be_taken");
    Check(loaded_textures == textures && loaded_samplers == samplers && loaded_spirv == spirv,
          "taken_translation_matches");
    Check(!cache.Has(7, 9, 1), "taken_translation_leaves_the_cache");
    Check(cache.Add(7, 9, 1, textures, samplers, spirv).empty(), "taken_translation_is_not_written_again");
  }
  {
    ShaderCache cache(43);
    cache.Load(file);
    Check(!cache.Has(7, 9, 1), "other_translator_fingerprint_drops_the_file");
    Check(cache.stale(), "dropped_file_is_reported_stale");
  }
  {
    ShaderCache cache(42);
    std::vector<uint8_t> truncated(file.begin(), file.end() - 3);
    cache.Load(truncated);
    Check(!cache.Has(7, 9, 1) && !cache.stale(), "torn_tail_record_is_ignored");
    Check(cache.valid_bytes() == cache.Header().size(), "torn_tail_is_cut_on_the_next_write");
  }
  return failures ? 1 : 0;
}
