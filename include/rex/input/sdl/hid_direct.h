#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rex::input::sdl {

/// Writes one output report (report id first) to the HID device at `path`
/// through its own handle, bypassing SDL's rumble queue. False on failure.
/// Windows only (hid_direct_win.cpp); callers guard it with _WIN32.
bool WriteHidOutputReport(const std::string& path, const uint8_t* report, size_t size);

}  // namespace rex::input::sdl
