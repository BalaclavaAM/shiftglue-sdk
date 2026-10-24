#include <catch2/catch_test_macros.hpp>

#include <rex/system/d3d_program.h>

TEST_CASE("draw program marks admit new lists after old marks expire",
          "[graphics][d3d_program]") {
  using namespace rex::system::d3d_program;
  // More unique addresses than the mark table holds, including full probe chains.
  for (uint32_t address = 1; address <= 65'536; ++address) MarkList(address);
  constexpr uint32_t kNewList = 0x10000000;
  MarkList(kNewList);
  CHECK_FALSE(IsList(kNewList));
  for (uint32_t frame = 0; frame < 9; ++frame) Swap();
  CHECK_FALSE(IsList(1));
  MarkList(kNewList);
  CHECK(IsList(kNewList));
  for (uint32_t frame = 0; frame < 9; ++frame) Swap();
  CHECK_FALSE(IsList(kNewList));
  MarkList(kNewList + 4);
  CHECK(IsList(kNewList + 4));
}
