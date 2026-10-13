/**
 * Output reports written straight to a HID device, outside SDL's rumble
 * queue. For the crash path: SDL writes one queued report every 10 ms or so,
 * and a pad busy with rumble can hold a release behind that queue until the
 * process is gone.
 */

#include <rex/input/sdl/hid_direct.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>

#include <algorithm>
#include <vector>

namespace rex::input::sdl {

bool WriteHidOutputReport(const std::string& path, const uint8_t* report, size_t size) {
  if (path.empty() || !report || !size) {
    return false;
  }
  // HID device paths are ASCII.
  const std::wstring wide(path.begin(), path.end());
  HANDLE device = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
  if (device == INVALID_HANDLE_VALUE) {
    return false;
  }
  // Windows takes writes of the device's longest output report; pad with
  // zeros as hidapi does. hid.dll is loaded on demand, as SDL does.
  size_t length = size;
  if (HMODULE hid = LoadLibraryW(L"hid.dll")) {
    using GetPreparsed = BOOLEAN(__stdcall*)(HANDLE, PHIDP_PREPARSED_DATA*);
    using FreePreparsed = BOOLEAN(__stdcall*)(PHIDP_PREPARSED_DATA);
    using GetCaps = NTSTATUS(__stdcall*)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
    auto get_preparsed =
        reinterpret_cast<GetPreparsed>(GetProcAddress(hid, "HidD_GetPreparsedData"));
    auto free_preparsed =
        reinterpret_cast<FreePreparsed>(GetProcAddress(hid, "HidD_FreePreparsedData"));
    auto get_caps = reinterpret_cast<GetCaps>(GetProcAddress(hid, "HidP_GetCaps"));
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (get_preparsed && free_preparsed && get_caps && get_preparsed(device, &preparsed)) {
      HIDP_CAPS caps = {};
      if (get_caps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
        length = std::max<size_t>(size, caps.OutputReportByteLength);
      }
      free_preparsed(preparsed);
    }
  }
  std::vector<uint8_t> buffer(length, 0);
  std::copy(report, report + size, buffer.begin());
  DWORD written = 0;
  const bool ok = WriteFile(device, buffer.data(), DWORD(buffer.size()), &written, nullptr) &&
                  written == buffer.size();
  CloseHandle(device);
  return ok;
}

}  // namespace rex::input::sdl
