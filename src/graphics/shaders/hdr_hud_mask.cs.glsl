#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_hdr: writes, into the guest output's alpha, how much of each pixel
// is the HUD, from how the frame differs from the snapshot taken before the
// HUD was drawn. The comparison does not depend on the channel order, which
// the swap source and the render target may not share.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform HdrHudMaskConstants {
  uvec2 size;
} constants;

layout(set = 0, binding = 0) uniform texture2D frame;
layout(set = 0, binding = 1) uniform texture2D scene;
layout(set = 0, binding = 2, rgb10_a2) uniform image2D guest_output;

float max3(vec3 v) { return max(v.r, max(v.g, v.b)); }
float min3(vec3 v) { return min(v.r, min(v.g, v.b)); }

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), constants.size))) {
    return;
  }
  vec3 frame_color = texelFetch(frame, pixel, 0).rgb;
  vec3 scene_color = texelFetch(scene, pixel, 0).rgb;
  float difference = abs(dot(frame_color - scene_color, vec3(1.0))) +
                     abs(max3(frame_color) - max3(scene_color)) +
                     abs(min3(frame_color) - min3(scene_color));
  // Above the rounding of an 8-bit copy.
  vec4 color = imageLoad(guest_output, pixel);
  color.a = smoothstep(0.01, 0.04, difference);
  imageStore(guest_output, pixel, color);
}
