#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::vulkan {

// Hand edits of the translator's SPIR-V for hot guest shaders (#413). The
// edits are code, not shader binaries: the translations come from the game's
// own microcode, so only their identities are listed here.
enum VulkanShaderEdit : uint32_t {
  // Drops the Xenos 0 * inf = 0 guards on products and sums
  // (OpSelect(OpIsNan(r), fallback, r) becomes r).
  kVulkanShaderEditDropNanGuards = 1u << 0,
  // Writes 0 for a NaN color output component at the end of main, so a NaN
  // the dropped guards would have zeroed never reaches blending or a resolve.
  kVulkanShaderEditGuardOutputs = 1u << 1,
};

// An edit applies only to the exact translation it was measured on: the
// guest ucode, the modification and the XXH3 hash of the translator's SPIR-V
// must all match, so a translator or host configuration change keeps the
// translation as is.
struct VulkanShaderOverride {
  xenos::ShaderType type;
  uint64_t ucode_data_hash;
  uint64_t modification;
  uint64_t translated_hash;
  uint32_t edits;
  const char* name;
};

std::span<const VulkanShaderOverride> GetBuiltInVulkanShaderOverrides();

// Applies edits (VulkanShaderEdit bits) to SPIR-V words. Returns false and
// leaves the words unchanged if the shader doesn't have the expected shape.
bool ApplyVulkanShaderEdits(std::vector<uint32_t>& words, uint32_t edits);

}  // namespace rex::graphics::vulkan
