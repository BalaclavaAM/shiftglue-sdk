#version 460

// UP-1.2: puts the game's HUD back over a temporal upscale of the HUD-less
// guest output. The HUD-less image's alpha is the HUD's coverage (the swap's
// HUD mask, already grown by a pixel for antialiased edges); where it is set,
// the output takes the guest output with the HUD, upscaled bilinearly, so the
// provider's history never holds the HUD.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform UpscaleHudCompositeConstants {
  uvec2 output_size;
  vec2 output_size_inv;
  // The share of the source images the upscaler read (its input size over
  // the image size).
  vec2 source_uv_scale;
} constants;

layout(set = 0, binding = 0) uniform sampler2D frame;
layout(set = 0, binding = 1) uniform sampler2D hudless;
layout(set = 0, binding = 2, rgb10_a2) uniform image2D upscaled;

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, constants.output_size))) {
    return;
  }
  vec2 uv = (vec2(pixel) + 0.5) * constants.output_size_inv * constants.source_uv_scale;
  float hud = textureLod(hudless, uv, 0.0).a;
  if (hud <= 0.0) {
    return;
  }
  vec4 color = imageLoad(upscaled, ivec2(pixel));
  color.rgb = mix(color.rgb, textureLod(frame, uv, 0.0).rgb, hud);
  imageStore(upscaled, ivec2(pixel), vec4(color.rgb, 1.0));
}
