#include <rex/graphics/cube_lut.h>

#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>

#include <rex/filesystem.h>

namespace rex::graphics {

namespace {

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool ParseFloats(std::string_view text, float* values, size_t count) {
  std::istringstream stream{std::string(text)};
  for (size_t i = 0; i < count; ++i) {
    if (!(stream >> values[i]) || !std::isfinite(values[i])) return false;
  }
  std::string rest;
  return !(stream >> rest);
}

}  // namespace

bool ParseCubeLut(std::string_view text, CubeLut& lut, std::string& error) {
  lut = {};
  size_t line_number = 0;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = Trim(text.substr(0, end));
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    ++line_number;
    if (line.empty() || line.front() == '#') continue;
    const auto fail = [&](std::string_view what) {
      error = "line " + std::to_string(line_number) + ": " + std::string(what);
      lut = {};
      return false;
    };
    if (line.starts_with("TITLE")) continue;
    if (line.starts_with("LUT_1D_SIZE")) return fail("1D tables are not supported");
    if (line.starts_with("LUT_3D_SIZE")) {
      std::string_view value = Trim(line.substr(11));
      uint32_t size = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), size);
      if (result.ec != std::errc() || result.ptr != value.data() + value.size() || size < 2 ||
          size > 256) {
        return fail("LUT_3D_SIZE must be 2 to 256");
      }
      if (lut.size) return fail("a second LUT_3D_SIZE");
      lut.size = size;
      lut.rgb.reserve(size_t(size) * size * size * 3);
      continue;
    }
    if (line.starts_with("DOMAIN_MIN") || line.starts_with("DOMAIN_MAX")) {
      const bool minimum = line.starts_with("DOMAIN_MIN");
      float domain[3];
      if (!ParseFloats(line.substr(10), domain, 3)) return fail("bad domain");
      const float expected = minimum ? 0.0f : 1.0f;
      if (domain[0] != expected || domain[1] != expected || domain[2] != expected) {
        return fail("only a 0 to 1 domain is supported");
      }
      continue;
    }
    if (!lut.size) return fail("data before LUT_3D_SIZE");
    float rgb[3];
    if (!ParseFloats(line, rgb, 3)) return fail("expected three numbers");
    if (lut.rgb.size() == size_t(lut.size) * lut.size * lut.size * 3) {
      return fail("more entries than LUT_3D_SIZE");
    }
    lut.rgb.insert(lut.rgb.end(), rgb, rgb + 3);
  }
  if (!lut.size) {
    error = "no LUT_3D_SIZE";
    return false;
  }
  if (lut.rgb.size() != size_t(lut.size) * lut.size * lut.size * 3) {
    error = "expected " + std::to_string(size_t(lut.size) * lut.size * lut.size) +
            " entries, found " + std::to_string(lut.rgb.size() / 3);
    lut = {};
    return false;
  }
  return true;
}

bool LoadCubeLut(const std::filesystem::path& path, CubeLut& lut, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open " + rex::path_to_utf8(path);
    return false;
  }
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  return ParseCubeLut(text, lut, error);
}

}  // namespace rex::graphics
