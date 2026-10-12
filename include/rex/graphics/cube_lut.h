#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rex::graphics {

// A 3D colour lookup table from a .cube file (the Adobe and DaVinci Resolve
// text format): size entries per axis, red varying fastest, then green, then
// blue, as the file lists them.
struct CubeLut {
  uint32_t size = 0;
  // size^3 RGB triples.
  std::vector<float> rgb;
};

// Accepts TITLE, LUT_3D_SIZE (2-256), DOMAIN_MIN and DOMAIN_MAX of 0 0 0 and
// 1 1 1, # comments and blank lines; refuses 1D tables and other domains.
bool ParseCubeLut(std::string_view text, CubeLut& lut, std::string& error);
bool LoadCubeLut(const std::filesystem::path& path, CubeLut& lut, std::string& error);

}  // namespace rex::graphics
