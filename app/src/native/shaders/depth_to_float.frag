#version 450

layout(set = 0, binding = 0) uniform sampler2D depth_source;

layout(push_constant) uniform Constants {
  ivec2 source_offset;
  ivec2 dest_offset;
  int scale_shift;
  int down_shift;
} constants;

layout(location = 0) out float depth_out;
layout(location = 1) out vec4 raw_out;

void main() {
  ivec2 position = ivec2(gl_FragCoord.xy) - constants.dest_offset;
  float depth = texelFetch(depth_source, constants.source_offset + ((position << constants.scale_shift) >> constants.down_shift), 0).r;
  depth_out = depth;
  uint bits = uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5) << 8;
  raw_out = vec4(uvec4(bits, bits >> 8, bits >> 16, bits >> 24) & 0xFFu) / 255.0;
}
