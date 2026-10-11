#include <rex/graphics/vulkan/shader_overrides.h>

#include <cstring>
#include <string_view>
#include <unordered_map>

namespace rex::graphics::vulkan {

namespace {

constexpr uint32_t kEdits = kVulkanShaderEditDropNanGuards | kVulkanShaderEditGuardOutputs;

// The hottest pixel shaders of the Odin drive and race (docs/ANDROID_ODIN_BACKLOG.md),
// as Turnip translates them with the default Android configuration.
constexpr VulkanShaderOverride kOverrides[] = {
    {xenos::ShaderType::kPixel, 0x9584B309533EF6C9, 0x00000000000E000F, 0x8C07D41D9C16BF3E,
     kEdits, "9584B309533EF6C9"},
    {xenos::ShaderType::kPixel, 0xE9CD565D9C61D037, 0x000000000016003F, 0x1EEDF998DF94921A,
     kEdits, "E9CD565D9C61D037"},
    {xenos::ShaderType::kPixel, 0x9D86C9913458AE3C, 0x00004000002B003F, 0x60D7FBB10DD21498,
     kEdits, "9D86C9913458AE3C"},
    {xenos::ShaderType::kPixel, 0x9D86C9913458AE3C, 0x00000000002B003F, 0x9B7B064118315DC9,
     kEdits, "9D86C9913458AE3C"},
    {xenos::ShaderType::kPixel, 0xBA6A2871A980A4E8, 0x0000000000000007, 0x195DE549A8758563,
     kEdits, "BA6A2871A980A4E8"},
    {xenos::ShaderType::kPixel, 0x614588022744BF6B, 0x0000400000000007, 0x2AE031375D6CEFBE,
     kEdits, "614588022744BF6B"},
};

enum Op : uint32_t {
  kOpName = 5,
  kOpTypeBool = 20,
  kOpTypeFloat = 22,
  kOpTypeVector = 23,
  kOpConstant = 43,
  kOpConstantComposite = 44,
  kOpFunction = 54,
  kOpVariable = 59,
  kOpLoad = 61,
  kOpStore = 62,
  kOpCopyObject = 83,
  kOpIsNan = 156,
  kOpSelect = 169,
  kOpReturn = 253,
};

constexpr uint32_t kHeaderWords = 5;

struct Instruction {
  size_t offset;
  uint32_t op;
  uint32_t count;
};

bool Parse(const std::vector<uint32_t>& words, std::vector<Instruction>& out) {
  if (words.size() < kHeaderWords || words[0] != 0x07230203u) return false;
  for (size_t i = kHeaderWords; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    if (!count || i + count > words.size()) return false;
    out.push_back({i, words[i] & 0xFFFFu, count});
    i += count;
  }
  return true;
}

std::string_view NameOf(const std::vector<uint32_t>& words, const Instruction& in) {
  const char* chars = reinterpret_cast<const char*>(words.data() + in.offset + 2);
  return std::string_view(chars, strnlen(chars, (in.count - 2) * sizeof(uint32_t)));
}

}  // namespace

std::span<const VulkanShaderOverride> GetBuiltInVulkanShaderOverrides() { return kOverrides; }

bool ApplyVulkanShaderEdits(std::vector<uint32_t>& words, uint32_t edits) {
  std::vector<Instruction> instructions;
  if (!edits || !Parse(words, instructions)) return false;

  // OpIsNan result -> its operand.
  std::unordered_map<uint32_t, uint32_t> is_nan;
  uint32_t f32 = 0, v4 = 0, boolean = 0, bv4 = 0, zero = 0, functions = 0, returns = 0;
  size_t first_variable = 0, last_type_or_zero = 0;
  std::vector<uint32_t> outputs;
  for (const Instruction& in : instructions) {
    const uint32_t* w = words.data() + in.offset;
    switch (in.op) {
      case kOpIsNan:
        if (in.count == 4) is_nan[w[2]] = w[3];
        break;
      case kOpTypeFloat:
        if (in.count == 3 && w[2] == 32) f32 = w[1], last_type_or_zero = in.offset;
        break;
      case kOpTypeBool:
        boolean = w[1];
        break;
      case kOpTypeVector:
        if (in.count == 4 && w[3] == 4 && w[2] == f32 && f32) v4 = w[1];
        if (in.count == 4 && w[3] == 4 && w[2] == boolean && boolean) bv4 = w[1];
        if (w[1] == v4 || w[1] == bv4) last_type_or_zero = std::max(last_type_or_zero, in.offset);
        break;
      case kOpConstant:
        if (in.count == 4 && w[1] == f32 && f32 && w[3] == 0 && !zero) {
          zero = w[2];
          last_type_or_zero = std::max(last_type_or_zero, in.offset);
        }
        break;
      case kOpName:
        if (NameOf(words, in).starts_with("xe_out_fragment_data_")) outputs.push_back(w[1]);
        break;
      case kOpVariable:
        if (!first_variable) first_variable = in.offset;
        break;
      case kOpFunction:
        ++functions;
        break;
      case kOpReturn:
        ++returns;
        break;
      default:
        break;
    }
  }
  const bool guard = (edits & kVulkanShaderEditGuardOutputs) != 0;
  if (guard && (!v4 || !bv4 || !zero || outputs.empty() || functions != 1 || returns != 1 ||
                !first_variable || last_type_or_zero > first_variable)) {
    return false;
  }

  uint32_t bound = words[3];
  const uint32_t zero4 = bound++;
  std::vector<uint32_t> out(words.begin(), words.begin() + kHeaderWords);
  out.reserve(words.size() + 8 + outputs.size() * 16);
  uint32_t dropped = 0;
  for (const Instruction& in : instructions) {
    const uint32_t* w = words.data() + in.offset;
    if (guard && in.offset == first_variable) {
      out.insert(out.end(), {(7u << 16) | kOpConstantComposite, v4, zero4, zero, zero, zero, zero});
    }
    if (in.op == kOpSelect && in.count == 6 && (edits & kVulkanShaderEditDropNanGuards)) {
      // OpSelect type result cond fallback value, with cond = OpIsNan(value).
      auto it = is_nan.find(w[3]);
      if (it != is_nan.end() && it->second == w[5]) {
        out.insert(out.end(), {(4u << 16) | kOpCopyObject, w[1], w[2], w[5]});
        ++dropped;
        continue;
      }
    }
    if (guard && in.op == kOpReturn) {
      for (uint32_t output : outputs) {
        const uint32_t loaded = bound++, nan = bound++, selected = bound++;
        out.insert(out.end(), {(4u << 16) | kOpLoad, v4, loaded, output});
        out.insert(out.end(), {(4u << 16) | kOpIsNan, bv4, nan, loaded});
        out.insert(out.end(), {(6u << 16) | kOpSelect, v4, selected, nan, zero4, loaded});
        out.insert(out.end(), {(3u << 16) | kOpStore, output, selected});
      }
    }
    out.insert(out.end(), w, w + in.count);
  }
  if ((edits & kVulkanShaderEditDropNanGuards) && !dropped) return false;
  out[3] = bound;
  words = std::move(out);
  return true;
}

}  // namespace rex::graphics::vulkan
