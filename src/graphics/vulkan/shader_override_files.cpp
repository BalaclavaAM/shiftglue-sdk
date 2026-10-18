#include <rex/graphics/vulkan/shader_override_files.h>

#include <cstring>
#include <fstream>
#include <optional>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <spirv-tools/libspirv.h>

namespace rex::graphics::vulkan {
namespace {

constexpr size_t kMaximumBytes = 16 * 1024 * 1024;

std::optional<uint32_t> MainExecutionModel(std::span<const uint32_t> words) {
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    if (!count || count > words.size() - i)
      return std::nullopt;
    if ((words[i] & 0xFFFF) == 15 && count >= 5 && words[i + 3] == 0x6E69616D && words[i + 4] == 0)
      return words[i + 1];  // OpEntryPoint ... "main"
    i += count;
  }
  return std::nullopt;
}

bool Validate(std::span<const uint8_t> bytes, std::span<const uint8_t> original,
              std::string& error) {
  if (bytes.size() < 20 || bytes.size() % 4 || bytes.size() > kMaximumBytes) {
    error = "invalid SPIR-V size";
    return false;
  }
  std::vector<uint32_t> words(bytes.size() / 4), original_words(original.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  std::memcpy(original_words.data(), original.data(), original_words.size() * 4);
  spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_2);
  if (!context) {
    error = "cannot create SPIR-V validator";
    return false;
  }
  spv_diagnostic diagnostic = nullptr;
  const auto result = spvValidateBinary(context, words.data(), words.size(), &diagnostic);
  if (result != SPV_SUCCESS)
    error = diagnostic ? diagnostic->error : "invalid SPIR-V";
  spvDiagnosticDestroy(diagnostic);
  spvContextDestroy(context);
  if (result != SPV_SUCCESS)
    return false;
  const auto model = MainExecutionModel(words);
  if (!model || model != MainExecutionModel(original_words)) {
    error = "main entry point does not match the translated stage";
    return false;
  }
  return true;
}

}  // namespace

std::string VulkanShaderOverrideFilename(uint64_t guest, uint64_t modification,
                                         std::span<const uint8_t> original) {
  return fmt::format("shader_{:016X}_{:016X}_{:016X}.spv", guest, modification,
                     XXH3_64bits(original.data(), original.size()));
}

std::vector<uint8_t> LoadVulkanShaderOverride(const std::vector<std::filesystem::path>& roots,
                                              uint64_t guest, uint64_t modification,
                                              std::span<const uint8_t> original,
                                              std::string& source,
                                              const std::function<void(const std::string&)>& warn) {
  source.clear();
  if (roots.empty()) return {};
  const auto filename = VulkanShaderOverrideFilename(guest, modification, original);
  for (const auto& root : roots) {
    const auto path = root / filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      std::error_code error;
      if (std::filesystem::exists(path, error) || error) {
        warn(fmt::format("{} rejected: cannot open SPIR-V", rex::path_to_utf8(path)));
      }
      continue;
    }
    const std::streamoff size = file.tellg();
    std::string error;
    std::vector<uint8_t> bytes;
    if (size < 20 || size > std::streamoff(kMaximumBytes) || size % 4) {
      error = "invalid SPIR-V size";
    } else {
      bytes.resize(size_t(size));
      file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        error = "cannot read SPIR-V";
      } else if (Validate(bytes, original, error)) {
        source = rex::path_to_utf8(path);
        return bytes;
      }
    }
    warn(fmt::format("{} rejected: {}", rex::path_to_utf8(path), error));
  }
  return {};
}

}  // namespace rex::graphics::vulkan
