#include <catch2/catch_test_macros.hpp>

#include <string>

#include <rex/graphics/cube_lut.h>

using rex::graphics::CubeLut;
using rex::graphics::ParseCubeLut;

TEST_CASE("A .cube LUT parses in file order", "[graphics]") {
  CubeLut lut;
  std::string error;
  const std::string text =
      "# A comment\r\nTITLE \"Swap\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n\n"
      "0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n";
  REQUIRE(ParseCubeLut(text, lut, error));
  CHECK(lut.size == 2);
  REQUIRE(lut.rgb.size() == 24);
  // The second entry is red = 1 in, blue = 1 out: a red/blue swap.
  CHECK(lut.rgb[3] == 0.0f);
  CHECK(lut.rgb[5] == 1.0f);
  CHECK(lut.rgb[21] == 1.0f);
}

TEST_CASE("A .cube LUT refuses what the pass cannot apply", "[graphics]") {
  CubeLut lut;
  std::string error;
  CHECK_FALSE(ParseCubeLut("LUT_1D_SIZE 4\n", lut, error));
  CHECK_FALSE(ParseCubeLut("LUT_3D_SIZE 2\nDOMAIN_MAX 2 2 2\n", lut, error));
  CHECK_FALSE(ParseCubeLut("LUT_3D_SIZE 1\n", lut, error));
  CHECK_FALSE(ParseCubeLut("0 0 0\n", lut, error));
  CHECK_FALSE(ParseCubeLut("LUT_3D_SIZE 2\n0 0 0\n", lut, error));
  CHECK(error == "expected 8 entries, found 1");
  CHECK_FALSE(ParseCubeLut("LUT_3D_SIZE 2\n0 0\n", lut, error));
  CHECK(lut.size == 0);
}
