#include "draw_state_filter.h"

#include <cstring>

namespace nier::native {

namespace {

template <typename T>
bool Same(const T& a, const T& b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

}  // namespace

uint32_t DynamicStateFilter::Changed(const DynamicState& next) {
  uint32_t bits = kAllDynamicState;
  if (valid_) {
    bits = 0;
    bits |= Same(next.viewport, last_.viewport) ? 0 : kViewportBit;
    bits |= Same(next.scissor, last_.scissor) ? 0 : kScissorBit;
    bits |= Same(next.depth_bias_constant, last_.depth_bias_constant) &&
                    Same(next.depth_bias_slope, last_.depth_bias_slope)
                ? 0
                : kDepthBiasBit;
    bits |= Same(next.blend, last_.blend) ? 0 : kBlendBit;
    bits |= next.compare_mask[0] == last_.compare_mask[0] ? 0 : kCompareMaskFrontBit;
    bits |= next.compare_mask[1] == last_.compare_mask[1] ? 0 : kCompareMaskBackBit;
    bits |= next.write_mask[0] == last_.write_mask[0] ? 0 : kWriteMaskFrontBit;
    bits |= next.write_mask[1] == last_.write_mask[1] ? 0 : kWriteMaskBackBit;
    bits |= next.reference[0] == last_.reference[0] ? 0 : kReferenceFrontBit;
    bits |= next.reference[1] == last_.reference[1] ? 0 : kReferenceBackBit;
  }
  last_ = next;
  valid_ = true;
  return bits;
}

uint32_t DescriptorBindingFilter::FirstChanged(const DescriptorBinding& next) {
  uint32_t first = 0;
  if (valid_) {
    first = kBoundSets;
    for (uint32_t i = kBoundSets; i-- > 0;) {
      if (next.sets[i] != last_.sets[i]) {
        first = i;
      }
    }
    if (next.offsets != last_.offsets && first > kConstantsSet) {
      first = kConstantsSet;
    }
    if (next.layout != last_.layout && first > kConstantsSet + 1) {
      first = kConstantsSet + 1;
    }
  }
  last_ = next;
  valid_ = true;
  return first;
}

bool DrawWritesNothing(uint32_t color_mask, rex::graphics::reg::RB_DEPTHCONTROL depth,
                       rex::graphics::reg::RB_STENCILREFMASK front, rex::graphics::reg::RB_STENCILREFMASK back) {
  using rex::graphics::xenos::StencilOp;
  if (color_mask || (depth.z_enable && depth.z_write_enable)) {
    return false;
  }
  if (!depth.stencil_enable) {
    return true;
  }
  const bool front_writes = front.stencilwritemask && (depth.stencilfail != StencilOp::kKeep ||
                                                       depth.stencilzpass != StencilOp::kKeep ||
                                                       depth.stencilzfail != StencilOp::kKeep);
  const bool back_writes = depth.backface_enable && back.stencilwritemask &&
                           (depth.stencilfail_bf != StencilOp::kKeep || depth.stencilzpass_bf != StencilOp::kKeep ||
                            depth.stencilzfail_bf != StencilOp::kKeep);
  return !front_writes && !back_writes;
}

bool EarlyTestsHelp(bool kills_pixels, bool writes_depth, rex::graphics::reg::RB_DEPTHCONTROL depth,
                    rex::graphics::reg::RB_STENCILREFMASK front, rex::graphics::reg::RB_STENCILREFMASK back) {
  return kills_pixels && !writes_depth && (depth.z_enable || depth.stencil_enable) &&
         DrawWritesNothing(0, depth, front, back);
}

bool RepeatedUniforms::Find(uint32_t stream, const void* data, size_t size, uint32_t& offset) const {
  const Entry& entry = entries_[stream];
  if (!entry.valid || entry.bytes.size() != size || std::memcmp(entry.bytes.data(), data, size) != 0) {
    return false;
  }
  offset = entry.offset;
  return true;
}

void RepeatedUniforms::Remember(uint32_t stream, const void* data, size_t size, uint32_t offset) {
  Entry& entry = entries_[stream];
  const auto* bytes = static_cast<const uint8_t*>(data);
  entry.bytes.assign(bytes, bytes + size);
  entry.offset = offset;
  entry.valid = true;
}

void RepeatedUniforms::Clear() {
  for (Entry& entry : entries_) {
    entry.valid = false;
  }
}

}  // namespace nier::native
