#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_hdr: the SDR stand-in (guest output and UI, sampled through its
// sRGB view, so linear) to the scRGB swapchain, where 1.0 is 80 nits. The
// stand-in's alpha is how much of the pixel is UI: the HUD of the guest output
// and the host UI stay at the paper white, and only the scene's highlights
// are expanded towards the peak brightness.

layout(push_constant) uniform HdrOutputConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) float paper_white_scale;  // Paper white nits / 80.
  float highlight_scale;  // Peak / paper white - 1, 0 for no expansion.
  float knee;             // Luminance where the expansion starts.
  float knee_range_inv;   // 1 / (1 - knee).
} constants;

layout(set = 0, binding = 0) uniform texture2D stand_in;

layout(location = 0) out vec4 output_color;

void main() {
  vec4 sdr = texelFetch(stand_in, ivec2(gl_FragCoord.xy), 0);
  float ui = clamp(sdr.a, 0.0, 1.0);
  float luminance = dot(sdr.rgb, vec3(0.2126, 0.7152, 0.0722));
  float t = clamp((luminance - constants.knee) * constants.knee_range_inv, 0.0, 1.0);
  // Smooth from the knee, reaching the peak at SDR white.
  float boost = 1.0 + constants.highlight_scale * t * t * (1.0 - ui);
  output_color = vec4(sdr.rgb * (constants.paper_white_scale * boost), 1.0);
}
