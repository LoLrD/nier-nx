#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nier::native {

class ShaderCache {
 public:
  explicit ShaderCache(uint64_t fingerprint) : fingerprint_(fingerprint) {}

  void Load(std::span<const uint8_t> file);
  std::vector<uint8_t> Header() const;
  bool Has(uint64_t ucode_hash, uint64_t modification, uint32_t variant) const;
  bool Take(uint64_t ucode_hash, uint64_t modification, uint32_t variant, std::vector<uint32_t>& textures,
            std::vector<uint32_t>& samplers, std::vector<uint8_t>& spirv);
  std::vector<uint8_t> Add(uint64_t ucode_hash, uint64_t modification, uint32_t variant,
                           std::span<const uint32_t> textures, std::span<const uint32_t> samplers,
                           std::span<const uint8_t> spirv);
  bool stale() const { return stale_; }
  size_t valid_bytes() const { return valid_bytes_; }
  size_t entries() const { return entries_.size(); }

 private:
  struct Key {
    uint64_t ucode_hash;
    uint64_t modification;
    uint32_t variant;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& key) const;
  };
  struct Entry {
    std::vector<uint32_t> textures;
    std::vector<uint32_t> samplers;
    std::vector<uint8_t> spirv;
  };

  uint64_t fingerprint_;
  std::unordered_map<Key, Entry, KeyHash> entries_;
  std::unordered_set<Key, KeyHash> recorded_;
  bool stale_ = false;
  size_t valid_bytes_ = 0;
};

}  // namespace nier::native
