/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "platform_win.h"

#include <rex/memory/utils.h>

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM | \
                            WINAPI_PARTITION_GAMES)
#define REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif

namespace rex {
namespace memory {

size_t page_size() {
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwPageSize;
  }
  return value;
}

size_t allocation_granularity() {
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwAllocationGranularity;
  }
  return value;
}

DWORD ToWin32ProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PAGE_NOACCESS;
    case PageAccess::kReadOnly:
      return PAGE_READONLY;
    case PageAccess::kReadWrite:
      return PAGE_READWRITE;
    case PageAccess::kExecuteReadOnly:
      return PAGE_EXECUTE_READ;
    case PageAccess::kExecuteReadWrite:
      return PAGE_EXECUTE_READWRITE;
    default:
      assert_unhandled_case(access);
      return PAGE_NOACCESS;
  }
}

PageAccess ToXeniaProtectFlags(DWORD access) {
  if (access & PAGE_GUARD) {
    // Strip the page guard flag for now...
    access &= ~PAGE_GUARD;
  }

  switch (access) {
    case PAGE_NOACCESS:
      return PageAccess::kNoAccess;
    case PAGE_READONLY:
      return PageAccess::kReadOnly;
    case PAGE_READWRITE:
      return PageAccess::kReadWrite;
    case PAGE_EXECUTE_READ:
      return PageAccess::kExecuteReadOnly;
    case PAGE_EXECUTE_READWRITE:
      return PageAccess::kExecuteReadWrite;
    default:
      return PageAccess::kNoAccess;
  }
}

bool IsWritableExecutableMemorySupported() {
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return true;
#else
  // To test FromApp functions on desktop, undefine
  // REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS and link to WindowsApp.lib.
  return false;
#endif
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  DWORD alloc_type = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      alloc_type = MEM_RESERVE;
      break;
    case AllocationType::kCommit:
      alloc_type = MEM_COMMIT;
      break;
    case AllocationType::kReserveCommit:
      alloc_type = MEM_RESERVE | MEM_COMMIT;
      break;
    default:
      assert_unhandled_case(allocation_type);
      break;
  }
  DWORD protect = ToWin32ProtectFlags(access);
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return VirtualAlloc(base_address, length, alloc_type, protect);
#else
  return VirtualAllocFromApp(base_address, length, ULONG(alloc_type), ULONG(protect));
#endif
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  DWORD free_type = 0;
  switch (deallocation_type) {
    case DeallocationType::kRelease:
      free_type = MEM_RELEASE;
      length = 0;
      break;
    case DeallocationType::kDecommit:
      free_type = MEM_DECOMMIT;
      break;
    default:
      assert_unhandled_case(deallocation_type);
      break;
  }
  return VirtualFree(base_address, length, free_type) ? true : false;
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }
  DWORD new_protect = ToWin32ProtectFlags(access);
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD old_protect = 0;
  BOOL result = VirtualProtect(base_address, length, new_protect, &old_protect);
#else
  ULONG old_protect = 0;
  BOOL result = VirtualProtectFromApp(base_address, length, ULONG(new_protect), &old_protect);
#endif
  if (!result) {
    return false;
  }
  if (out_old_access) {
    *out_old_access = ToXeniaProtectFlags(DWORD(old_protect));
  }
  return true;
}

bool IsHostReadable(const void* address) {
  size_t length = page_size();
  PageAccess access = PageAccess::kNoAccess;
  return QueryProtect(const_cast<void*>(address), length, access) &&
         access != PageAccess::kNoAccess;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;

  MEMORY_BASIC_INFORMATION info;
  ZeroMemory(&info, sizeof(info));

  SIZE_T result = VirtualQuery(base_address, &info, sizeof(info));
  if (!result) {
    return false;
  }

  length = info.RegionSize;
  access_out = ToXeniaProtectFlags(info.Protect);
  return true;
}

#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
namespace {

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif

// VirtualAlloc2 and MapViewOfFile3 (Windows 10 1803 and later) map a view at
// any page-aligned file offset, where MapViewOfFileEx rounds the offset to the
// 64 KB allocation granularity. The guest's 0xE0000000 physical view starts
// 4 KB into the physical heap; mapping it there removes the +0x1000 select
// from every generated guest memory access (NEXT_LEVEL_RESEARCH.md NL-1.1).
// Resolved at run time so the binary still loads on an older system, which
// Memory::Initialize then refuses with a message instead of running with the
// wrong view.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE process, PVOID base_address, SIZE_T size,
                                       ULONG allocation_type, ULONG page_protection,
                                       void* extended_parameters, ULONG parameter_count);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE file_mapping, HANDLE process, PVOID base_address,
                                        ULONG64 offset, SIZE_T view_size, ULONG allocation_type,
                                        ULONG page_protection, void* extended_parameters,
                                        ULONG parameter_count);

struct PlaceholderViewApi {
  VirtualAlloc2Fn virtual_alloc2 = nullptr;
  MapViewOfFile3Fn map_view_of_file3 = nullptr;
  bool available() const { return virtual_alloc2 && map_view_of_file3; }
};

const PlaceholderViewApi& placeholder_view_api() {
  static const PlaceholderViewApi api = [] {
    PlaceholderViewApi resolved;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelbase) {
      kernelbase = LoadLibraryW(L"kernelbase.dll");
    }
    if (kernelbase) {
      resolved.virtual_alloc2 =
          reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelbase, "VirtualAlloc2"));
      resolved.map_view_of_file3 =
          reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelbase, "MapViewOfFile3"));
    }
    return resolved;
  }();
  return api;
}

}  // namespace
#endif

bool MapFileViewOffsetsAreExact() {
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return placeholder_view_api().available();
#else
  // MapViewOfFile3FromApp takes any page-aligned offset.
  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  DWORD protect = ToWin32ProtectFlags(access) | (commit ? SEC_COMMIT : SEC_RESERVE);
  auto full_path = "Local" / path;
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  HANDLE h =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, protect, static_cast<DWORD>(length >> 32),
                         static_cast<DWORD>(length), full_path.c_str());
#else
  HANDLE h = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, ULONG(protect),
                                      ULONG64(length), full_path.c_str());
#endif
  if (!h) {
    return kFileMappingHandleInvalid;
  }
  return reinterpret_cast<FileMappingHandle>(h);
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  CloseHandle(reinterpret_cast<HANDLE>(handle));
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  const PlaceholderViewApi& placeholders = placeholder_view_api();
  if (placeholders.available()) {
    // A placeholder reservation replaced by the view: the only way to map a
    // view at a file offset that is page-aligned but not 64 KB-aligned.
    HANDLE process = GetCurrentProcess();
    void* placeholder = placeholders.virtual_alloc2(
        process, base_address, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
        nullptr, 0);
    if (!placeholder) {
      return nullptr;
    }
    void* mapping = placeholders.map_view_of_file3(
        reinterpret_cast<HANDLE>(handle), process, placeholder, ULONG64(file_offset), length,
        MEM_REPLACE_PLACEHOLDER, ULONG(ToWin32ProtectFlags(access)), nullptr, 0);
    if (!mapping) {
      VirtualFree(placeholder, 0, MEM_RELEASE);
      return nullptr;
    }
    return mapping;
  }
  DWORD target_address_low = static_cast<DWORD>(file_offset);
  DWORD target_address_high = static_cast<DWORD>(file_offset >> 32);
  DWORD file_access = 0;
  switch (access) {
    case PageAccess::kReadOnly:
      file_access = FILE_MAP_READ;
      break;
    case PageAccess::kReadWrite:
      file_access = FILE_MAP_ALL_ACCESS;
      break;
    case PageAccess::kExecuteReadOnly:
      file_access = FILE_MAP_READ | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kExecuteReadWrite:
      file_access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kNoAccess:
    default:
      assert_unhandled_case(access);
      return nullptr;
  }
  return MapViewOfFileEx(reinterpret_cast<HANDLE>(handle), file_access, target_address_high,
                         target_address_low, length, base_address);
#else
  // VirtualAlloc2FromApp and MapViewOfFile3FromApp were added in 10.0.17134.0.
  // https://docs.microsoft.com/en-us/uwp/win32-and-com/win32-apis
  HANDLE process = GetCurrentProcess();
  void* placeholder =
      VirtualAlloc2FromApp(process, base_address, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                           PAGE_NOACCESS, nullptr, 0);
  if (!placeholder) {
    return nullptr;
  }
  void* mapping = MapViewOfFile3FromApp(reinterpret_cast<HANDLE>(handle), process, placeholder,
                                        ULONG64(file_offset), length, MEM_REPLACE_PLACEHOLDER,
                                        ULONG(ToWin32ProtectFlags(access)), nullptr, 0);
  if (!mapping) {
    VirtualFree(placeholder, length, MEM_RELEASE);
    return nullptr;
  }
  return mapping;
#endif
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  return UnmapViewOfFile(base_address) ? true : false;
}

}  // namespace memory
}  // namespace rex
