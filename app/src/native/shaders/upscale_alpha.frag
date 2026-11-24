#version 450

layout(set = 0, binding = 0) uniform texture2D source_texture;
layout(set = 0, binding = 1) uniform sampler linear_sampler;

layout(push_constant) uniform Constants {
  layout(offset = 16) vec2 inverse_output_size;
} constants;

layout(location = 0) out vec4 color;

void main() {
  const vec2 coordinates = gl_FragCoord.xy * constants.inverse_output_size;
  color = vec4(0.0, 0.0, 0.0, texture(sampler2D(source_texture, linear_sampler), coordinates).a);
}
