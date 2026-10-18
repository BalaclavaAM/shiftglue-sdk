#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace rex::graphics::vulkan {

std::string VulkanShaderOverrideFilename(uint64_t guest, uint64_t modification,
                                         std::span<const uint8_t> original);

// First valid exact-identity file wins. Bad files never reach the driver.
std::vector<uint8_t> LoadVulkanShaderOverride(const std::vector<std::filesystem::path>& roots,
                                              uint64_t guest, uint64_t modification,
                                              std::span<const uint8_t> original,
                                              std::string& source,
                                              const std::function<void(const std::string&)>& warn);

}  // namespace rex::graphics::vulkan
