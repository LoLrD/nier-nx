#include "phased_hash.h"

#include <algorithm>

#include <rex/hash.h>

namespace nier::native {

namespace {

uint64_t Mix(uint64_t state, uint64_t value) {
  state = (state ^ value) * 0x9E3779B97F4A7C15ull;
  return state ^ (state >> 29);
}

template <typename Visit>
void ForEachBlock(std::span<const HashSpan> spans, Visit&& visit) {
  uint32_t index = 0;
  for (const HashSpan& span : spans) {
    for (uint32_t offset = 0; offset < span.bytes; offset += PhasedHash::kBlockBytes, ++index) {
      visit(index, span.data + offset, std::min(PhasedHash::kBlockBytes, span.bytes - offset));
    }
  }
}

}  // namespace

uint32_t PhasedHash::Total(std::span<const HashSpan> spans) {
  uint32_t total = 0;
  for (const HashSpan& span : spans) {
    total += span.bytes;
  }
  return total;
}

uint64_t PhasedHash::Edges(std::span<const HashSpan> spans, uint32_t total) {
  uint64_t edges = total;
  for (const HashSpan& span : spans) {
    if (!span.bytes) {
      continue;
    }
    const uint32_t first = std::min(kBlockBytes, span.bytes);
    const uint32_t last = std::min(kBlockBytes, span.bytes);
    edges = Mix(edges, XXH3_64bits(span.data, first));
    edges = Mix(edges, XXH3_64bits(span.data + span.bytes - last, last));
  }
  return edges;
}

PhasedHash::Digest PhasedHash::Full(std::span<const HashSpan> spans, uint32_t total) {
  Digest digest;
  if (total <= kMinSampledBytes) {
    for (const HashSpan& span : spans) {
      digest.phases[0] = Mix(digest.phases[0], XXH3_64bits(span.data, span.bytes) + span.bytes);
    }
    return digest;
  }
  digest.edges = Edges(spans, total);
  ForEachBlock(spans, [&](uint32_t index, const uint8_t* data, uint32_t bytes) {
    uint64_t& phase = digest.phases[index % kPhases];
    phase = Mix(phase, XXH3_64bits(data, bytes) + index);
  });
  return digest;
}

uint64_t PhasedHash::Phase(std::span<const HashSpan> spans, uint32_t phase, uint32_t& hashed) {
  uint64_t result = 0;
  ForEachBlock(spans, [&](uint32_t index, const uint8_t* data, uint32_t bytes) {
    if (index % kPhases == phase) {
      result = Mix(result, XXH3_64bits(data, bytes) + index);
      hashed += bytes;
    }
  });
  return result;
}

uint32_t PhasedHash::Reset(std::span<const HashSpan> spans) {
  const uint32_t total = Total(spans);
  digest_ = Full(spans, total);
  unchanged_ = 0;
  return total;
}

PhasedHash::Result PhasedHash::Check(std::span<const HashSpan> spans) {
  const uint32_t total = Total(spans);
  if (sampling() && total > kMinSampledBytes) {
    const uint32_t phase = next_phase_;
    next_phase_ = uint8_t((next_phase_ + 1) % kPhases);
    uint32_t hashed = 0;
    const uint64_t edges = Edges(spans, total);
    hashed += std::min(total, 2 * kBlockBytes * uint32_t(spans.size()));
    if (edges == digest_.edges && Phase(spans, phase, hashed) == digest_.phases[phase]) {
      return {false, true, hashed};
    }
    digest_ = Full(spans, total);
    volatile_ = true;
    unchanged_ = 0;
    return {true, true, hashed + total};
  }
  const Digest digest = Full(spans, total);
  if (digest == digest_) {
    unchanged_ = uint8_t(std::min<uint32_t>(unchanged_ + 1, kStableChecks));
    return {false, false, total};
  }
  digest_ = digest;
  volatile_ = true;
  unchanged_ = 0;
  return {true, false, total};
}

bool PhasedHash::Matches(std::span<const HashSpan> spans) const { return Full(spans, Total(spans)) == digest_; }

}  // namespace nier::native
