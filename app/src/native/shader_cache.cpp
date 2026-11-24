#include "shader_cache.h"

#include <cstring>

namespace nier::native {

namespace {

constexpr uint32_t kMagic = 0x3143534E;
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 16;
constexpr size_t kRecordHeaderBytes = 32;

template <typename T>
void Put(std::vector<uint8_t>& out, T value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
T Get(std::span<const uint8_t> file, size_t at) {
  T value;
  std::memcpy(&value, file.data() + at, sizeof(T));
  return value;
}

}  // namespace

size_t ShaderCache::KeyHash::operator()(const Key& key) const {
  return size_t(key.ucode_hash * 0x9E3779B97F4A7C15ull ^ key.modification * 0xC2B2AE3D27D4EB4Full ^ key.variant);
}

std::vector<uint8_t> ShaderCache::Header() const {
  std::vector<uint8_t> out;
  Put(out, kMagic);
  Put(out, kVersion);
  Put(out, fingerprint_);
  return out;
}

void ShaderCache::Load(std::span<const uint8_t> file) {
  entries_.clear();
  recorded_.clear();
  stale_ = false;
  valid_bytes_ = kHeaderBytes;
  if (file.size() < kHeaderBytes) {
    stale_ = !file.empty();
    return;
  }
  if (Get<uint32_t>(file, 0) != kMagic || Get<uint32_t>(file, 4) != kVersion || Get<uint64_t>(file, 8) != fingerprint_) {
    stale_ = true;
    return;
  }
  size_t at = kHeaderBytes;
  while (at + kRecordHeaderBytes <= file.size()) {
    const Key key{Get<uint64_t>(file, at), Get<uint64_t>(file, at + 8), Get<uint32_t>(file, at + 16)};
    const uint32_t textures = Get<uint32_t>(file, at + 20);
    const uint32_t samplers = Get<uint32_t>(file, at + 24);
    const uint32_t spirv = Get<uint32_t>(file, at + 28);
    const size_t size = kRecordHeaderBytes + (size_t(textures) + samplers) * 4 + spirv;
    if (at + size > file.size()) {
      break;
    }
    Entry entry;
    const size_t textures_at = at + kRecordHeaderBytes;
    const size_t samplers_at = textures_at + size_t(textures) * 4;
    const size_t spirv_at = samplers_at + size_t(samplers) * 4;
    entry.textures.resize(textures);
    std::memcpy(entry.textures.data(), file.data() + textures_at, size_t(textures) * 4);
    entry.samplers.resize(samplers);
    std::memcpy(entry.samplers.data(), file.data() + samplers_at, size_t(samplers) * 4);
    entry.spirv.assign(file.begin() + spirv_at, file.begin() + spirv_at + spirv);
    entries_[key] = std::move(entry);
    recorded_.insert(key);
    at += size;
  }
  valid_bytes_ = at;
}

bool ShaderCache::Has(uint64_t ucode_hash, uint64_t modification, uint32_t variant) const {
  return entries_.contains(Key{ucode_hash, modification, variant});
}

bool ShaderCache::Take(uint64_t ucode_hash, uint64_t modification, uint32_t variant, std::vector<uint32_t>& textures,
                       std::vector<uint32_t>& samplers, std::vector<uint8_t>& spirv) {
  const auto it = entries_.find(Key{ucode_hash, modification, variant});
  if (it == entries_.end()) {
    return false;
  }
  textures = std::move(it->second.textures);
  samplers = std::move(it->second.samplers);
  spirv = std::move(it->second.spirv);
  entries_.erase(it);
  return true;
}

std::vector<uint8_t> ShaderCache::Add(uint64_t ucode_hash, uint64_t modification, uint32_t variant,
                                      std::span<const uint32_t> textures, std::span<const uint32_t> samplers,
                                      std::span<const uint8_t> spirv) {
  if (!recorded_.insert(Key{ucode_hash, modification, variant}).second) {
    return {};
  }
  std::vector<uint8_t> out;
  out.reserve(kRecordHeaderBytes + (textures.size() + samplers.size()) * 4 + spirv.size());
  Put(out, ucode_hash);
  Put(out, modification);
  Put(out, variant);
  Put(out, uint32_t(textures.size()));
  Put(out, uint32_t(samplers.size()));
  Put(out, uint32_t(spirv.size()));
  for (const uint32_t value : textures) {
    Put(out, value);
  }
  for (const uint32_t value : samplers) {
    Put(out, value);
  }
  out.insert(out.end(), spirv.begin(), spirv.end());
  return out;
}

}  // namespace nier::native
