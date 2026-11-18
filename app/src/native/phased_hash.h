#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace nier::native {

struct HashSpan {
  const uint8_t* data;
  uint32_t bytes;
};

class PhasedHash {
 public:
  static constexpr uint32_t kBlockBytes = 4096;
  static constexpr uint32_t kPhases = 8;
  static constexpr uint32_t kStableChecks = 8;
  static constexpr uint32_t kMinSampledBytes = 64 * 1024;

  struct Result {
    bool changed;
    bool sampled;
    bool possibly_late;
    uint32_t hashed_bytes;
  };

  uint32_t Reset(std::span<const HashSpan> spans);
  Result Check(std::span<const HashSpan> spans);
  bool Matches(std::span<const HashSpan> spans) const;
  void MarkVolatile() { volatile_ = true; }
  bool sampling() const { return !volatile_ && unchanged_ >= kStableChecks; }

 private:
  struct Digest {
    std::array<uint64_t, kPhases> phases{};
    uint64_t edges = 0;
    bool operator==(const Digest&) const = default;
  };

  static uint32_t Total(std::span<const HashSpan> spans);
  static uint64_t Edges(std::span<const HashSpan> spans, uint32_t total);
  static Digest Full(std::span<const HashSpan> spans, uint32_t total);
  static uint64_t Phase(std::span<const HashSpan> spans, uint32_t phase, uint32_t& hashed);

  Digest digest_;
  uint8_t next_phase_ = 0;
  uint8_t unchanged_ = 0;
  bool volatile_ = false;
};

}  // namespace nier::native
