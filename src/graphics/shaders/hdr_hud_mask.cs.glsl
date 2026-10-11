#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_hdr: writes, into the guest output's alpha, whether each pixel is
// the HUD, from how the frame differs from the snapshot taken before the HUD
// was drawn. The comparison does not depend on the channel order, which the
// swap source and the render target may not share: it compares the sorted
// channels.
//
// The guest output's alpha has 2 bits, so a partial coverage would band and
// flicker where a translucent HUD element animates (the prestige meter, #442).
// Any change above the rounding of an 8-bit copy counts as HUD instead, and
// the mask grows by a pixel to cover the HUD's antialiased edges, whose
// changes can be smaller.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform HdrHudMaskConstants {
  uvec2 size;
} constants;

layout(set = 0, binding = 0) uniform texture2D frame;
layout(set = 0, binding = 1) uniform texture2D scene;
layout(set = 0, binding = 2, rgb10_a2) uniform image2D guest_output;

vec3 Sorted(vec3 v) {
  float low = min(v.r, min(v.g, v.b));
  float high = max(v.r, max(v.g, v.b));
  return vec3(low, v.r + v.g + v.b - low - high, high);
}

bool Changed(ivec2 pixel) {
  pixel = clamp(pixel, ivec2(0), ivec2(constants.size) - 1);
  vec3 difference = abs(Sorted(texelFetch(frame, pixel, 0).rgb) -
                        Sorted(texelFetch(scene, pixel, 0).rgb));
  // Half an 8-bit step is 0.002.
  return max(difference.x, max(difference.y, difference.z)) > 0.006;
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), constants.size))) {
    return;
  }
  bool hud = false;
  for (int y = -1; y <= 1 && !hud; ++y) {
    for (int x = -1; x <= 1 && !hud; ++x) {
      hud = Changed(pixel + ivec2(x, y));
    }
  }
  vec4 color = imageLoad(guest_output, pixel);
  color.a = hud ? 1.0 : 0.0;
  imageStore(guest_output, pixel, color);
}
