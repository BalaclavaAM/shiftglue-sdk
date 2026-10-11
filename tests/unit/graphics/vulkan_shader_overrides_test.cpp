#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

#include <rex/graphics/vulkan/shader_overrides.h>

using namespace rex::graphics::vulkan;

namespace {

constexpr uint32_t Op(uint32_t op, uint32_t count) { return (count << 16) | op; }

void Add(std::vector<uint32_t>& words, std::initializer_list<uint32_t> instruction) {
  words.insert(words.end(), instruction);
}

// A fragment shader with one guarded product written to the color output:
// %14 = OpSelect(OpIsNan(%12), %12, %12).
std::vector<uint32_t> GuardedModule() {
  std::vector<uint32_t> words = {0x07230203u, 0x00010500u, 0, 16, 0};
  uint32_t name[6] = {};
  std::memcpy(name, "xe_out_fragment_data_0", 22);
  words.push_back(Op(5, 2 + 6));
  words.push_back(7);
  words.insert(words.end(), std::begin(name), std::end(name));
  Add(words, {Op(22, 3), 1, 32});         // %1 = OpTypeFloat 32
  Add(words, {Op(23, 4), 2, 1, 4});       // %2 = OpTypeVector %1 4
  Add(words, {Op(20, 2), 3});             // %3 = OpTypeBool
  Add(words, {Op(23, 4), 4, 3, 4});       // %4 = OpTypeVector %3 4
  Add(words, {Op(43, 4), 1, 5, 0});       // %5 = OpConstant %1 0
  Add(words, {Op(32, 4), 6, 3, 2});       // %6 = OpTypePointer Output %2
  Add(words, {Op(59, 4), 6, 7, 3});       // %7 = OpVariable %6 Output
  Add(words, {Op(19, 2), 8});             // %8 = OpTypeVoid
  Add(words, {Op(33, 3), 9, 8});          // %9 = OpTypeFunction %8
  Add(words, {Op(54, 5), 8, 10, 0, 9});   // %10 = OpFunction
  Add(words, {Op(248, 2), 11});           // %11 = OpLabel
  Add(words, {Op(61, 4), 2, 12, 7});      // %12 = OpLoad %2 %7
  Add(words, {Op(156, 4), 4, 13, 12});    // %13 = OpIsNan %4 %12
  Add(words, {Op(169, 6), 2, 14, 13, 12, 12});  // %14 = OpSelect
  Add(words, {Op(62, 3), 7, 14});         // OpStore %7 %14
  Add(words, {Op(253, 1)});               // OpReturn
  Add(words, {Op(56, 1)});                // OpFunctionEnd
  return words;
}

size_t Count(const std::vector<uint32_t>& words, uint32_t first_word) {
  return std::count(words.begin() + 5, words.end(), first_word);
}

}  // namespace

TEST_CASE("vulkan shader edits drop guards and guard the outputs") {
  std::vector<uint32_t> words = GuardedModule();
  const size_t size = words.size();
  REQUIRE(ApplyVulkanShaderEdits(
      words, kVulkanShaderEditDropNanGuards | kVulkanShaderEditGuardOutputs));
  // The guarded select became a copy of the product.
  const std::vector<uint32_t> copy = {Op(83, 4), 2, 14, 12};
  CHECK(std::search(words.begin(), words.end(), copy.begin(), copy.end()) != words.end());
  // A zero vector constant and one load, NaN test, select and store before
  // the return.
  CHECK(Count(words, Op(44, 7)) == 1);
  CHECK(Count(words, Op(156, 4)) == 2);
  CHECK(Count(words, Op(169, 6)) == 1);
  CHECK(Count(words, Op(62, 3)) == 2);
  CHECK(words.size() == size - 2 + 7 + 4 + 4 + 6 + 3);
  CHECK(words[3] == 16 + 4);
  CHECK(words[words.size() - 2] == Op(253, 1));
}

TEST_CASE("vulkan shader edits keep shaders of another shape") {
  std::vector<uint32_t> words = GuardedModule();
  // No guard to drop: the select's value is not the tested one.
  words[words.size() - 6] = 13;
  const std::vector<uint32_t> before = words;
  CHECK_FALSE(ApplyVulkanShaderEdits(words, kVulkanShaderEditDropNanGuards));
  CHECK(words == before);

  std::vector<uint32_t> not_spirv = {1, 2, 3, 4, 5};
  CHECK_FALSE(ApplyVulkanShaderEdits(not_spirv, kVulkanShaderEditDropNanGuards));
}

TEST_CASE("built-in vulkan shader overrides name exact translations") {
  for (const VulkanShaderOverride& entry : GetBuiltInVulkanShaderOverrides()) {
    CHECK(entry.translated_hash != 0);
    CHECK(entry.edits != 0);
  }
}
