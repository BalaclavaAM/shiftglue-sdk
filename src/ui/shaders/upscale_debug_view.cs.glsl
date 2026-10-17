#version 460

// present_upscaler_view: replaces a temporal upscale with one of its inputs,
// to check them. Depth is shown as linear distance on a log scale (white
// near, black at the far plane); motion as red and green around grey, one
// pixel of motion at the render size being one step of `motion_gain`.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform UpscaleDebugViewConstants {
  uvec2 output_size;
  vec2 output_size_inv;
  vec2 source_uv_scale;
  vec2 render_size;
  float near_plane;
  float far_plane;
  float motion_gain;
  // 1 depth, 2 motion.
  uint mode;
} constants;

layout(set = 0, binding = 0) uniform sampler2D depth;
layout(set = 0, binding = 1) uniform sampler2D motion;
layout(set = 0, binding = 2, rgb10_a2) uniform writeonly image2D dest;

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, constants.output_size))) {
    return;
  }
  vec2 uv = (vec2(pixel) + 0.5) * constants.output_size_inv * constants.source_uv_scale;
  vec3 color;
  if (constants.mode == 1u) {
    float z = textureLod(depth, uv, 0.0).r;
    float n = constants.near_plane;
    float f = constants.far_plane;
    // D3D projection: z = f / (f - n) - f n / ((f - n) d).
    float distance = (f * n) / max(f - z * (f - n), 1.0e-6);
    float shade = 1.0 - clamp(log2(max(distance / n, 1.0)) / log2(f / n), 0.0, 1.0);
    color = vec3(shade);
    if (z > 1.0 || z < 0.0) {
      color = vec3(1.0, 0.0, 1.0);
    }
  } else {
    vec2 pixels = textureLod(motion, uv, 0.0).rg * constants.render_size;
    color = vec3(clamp(0.5 + pixels * constants.motion_gain, 0.0, 1.0), 0.5);
  }
  imageStore(dest, ivec2(pixel), vec4(color, 1.0));
}
