#include <cstdint>
#include <cstdio>

#include "draw_state_filter.h"

using nier::native::DescriptorBinding;
using nier::native::DescriptorBindingFilter;
using nier::native::DynamicState;
using nier::native::DrawWritesNothing;
using nier::native::DynamicStateFilter;
using nier::native::RepeatedUniforms;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static VkDescriptorSet Set(uintptr_t value) { return reinterpret_cast<VkDescriptorSet>(value); }
static VkPipelineLayout Layout(uintptr_t value) { return reinterpret_cast<VkPipelineLayout>(value); }

int main() {
  {
    DynamicStateFilter filter;
    DynamicState state;
    state.viewport = {0.0f, 0.0f, 640.0f, 360.0f, 0.0f, 1.0f};
    Check(filter.Changed(state) == nier::native::kAllDynamicState, "first_state_sets_everything");
    Check(filter.Changed(state) == 0, "repeated_state_sets_nothing");
    state.scissor.extent = {320, 180};
    state.reference[1] = 7;
    Check(filter.Changed(state) == (nier::native::kScissorBit | nier::native::kReferenceBackBit),
          "only_changed_fields_are_set");
    filter.Reset();
    Check(filter.Changed(state) == nier::native::kAllDynamicState, "reset_sets_everything_again");
  }
  {
    DynamicStateFilter filter;
    DynamicState state;
    state.depth_bias_constant = -0.0f;
    filter.Changed(state);
    state.depth_bias_constant = 0.0f;
    Check(filter.Changed(state) == nier::native::kDepthBiasBit, "float_state_compared_bitwise");
  }
  {
    DescriptorBindingFilter filter;
    DescriptorBinding binding{Layout(1), {Set(10), Set(11), Set(12), Set(13)}, {0, 256, 512, 768, 1024}};
    Check(filter.FirstChanged(binding) == 0, "first_binding_binds_all_sets");
    Check(filter.FirstChanged(binding) == nier::native::kBoundSets, "repeated_binding_binds_nothing");
    binding.sets[3] = Set(23);
    Check(filter.FirstChanged(binding) == 3, "changed_pixel_textures_bind_from_last_set");
    binding.offsets[4] = 2048;
    binding.sets[2] = Set(22);
    Check(filter.FirstChanged(binding) == 1, "changed_offsets_bind_from_constants_set");
    binding.layout = Layout(2);
    Check(filter.FirstChanged(binding) == 2, "changed_layout_rebinds_texture_sets");
    binding.sets[0] = Set(30);
    Check(filter.FirstChanged(binding) == 0, "changed_shared_set_binds_all");
    filter.Reset();
    Check(filter.FirstChanged(binding) == 0, "reset_binds_all_again");
  }
  {
    RepeatedUniforms uniforms;
    const uint32_t a[4] = {1, 2, 3, 4};
    const uint32_t b[4] = {1, 2, 3, 5};
    uint32_t offset = 0;
    Check(!uniforms.Find(0, a, sizeof(a), offset), "unknown_uniform_not_found");
    uniforms.Remember(0, a, sizeof(a), 256);
    Check(uniforms.Find(0, a, sizeof(a), offset) && offset == 256, "same_bytes_reuse_offset");
    Check(!uniforms.Find(0, b, sizeof(b), offset), "different_bytes_not_reused");
    Check(!uniforms.Find(0, a, sizeof(a) - 4, offset), "different_size_not_reused");
    Check(!uniforms.Find(1, a, sizeof(a), offset), "streams_are_separate");
    uniforms.Clear();
    Check(!uniforms.Find(0, a, sizeof(a), offset), "cleared_uniform_not_found");
  }
  {
    using rex::graphics::reg::RB_DEPTHCONTROL;
    using rex::graphics::reg::RB_STENCILREFMASK;
    RB_DEPTHCONTROL depth{};
    depth.z_enable = 1;
    RB_STENCILREFMASK stencil{};
    stencil.stencilwritemask = 0xFF;
    Check(DrawWritesNothing(0, depth, stencil, stencil), "depth_test_only_without_color_writes_nothing");
    Check(!DrawWritesNothing(0xF, depth, stencil, stencil), "color_write_is_a_write");
    depth.z_write_enable = 1;
    Check(!DrawWritesNothing(0, depth, stencil, stencil), "depth_write_is_a_write");
    depth.z_write_enable = 0;
    depth.stencil_enable = 1;
    Check(DrawWritesNothing(0, depth, stencil, stencil), "stencil_with_keep_ops_writes_nothing");
    depth.stencilzpass = rex::graphics::xenos::StencilOp::kReplace;
    Check(!DrawWritesNothing(0, depth, stencil, stencil), "stencil_replace_is_a_write");
    stencil.stencilwritemask = 0;
    Check(DrawWritesNothing(0, depth, stencil, stencil), "stencil_write_mask_zero_writes_nothing");
    depth.stencilzpass = rex::graphics::xenos::StencilOp::kKeep;
    depth.backface_enable = 1;
    depth.stencilfail_bf = rex::graphics::xenos::StencilOp::kIncrementClamp;
    RB_STENCILREFMASK back{};
    back.stencilwritemask = 0xFF;
    Check(!DrawWritesNothing(0, depth, stencil, back), "back_face_stencil_op_is_a_write");
  }
  return failures ? 1 : 0;
}
