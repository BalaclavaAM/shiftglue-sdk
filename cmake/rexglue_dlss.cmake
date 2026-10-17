# rexglue_dlss.cmake — the NVIDIA NGX headers and loader library for the DLSS
# provider (src/ui/vulkan/vulkan_dlss_upscaler.cpp).
#
# Off by default. NVIDIA's RTX SDK licence allows the NGX loader inside an
# application as object code, but the SDK must not be made subject to an
# open-source licence, so nothing of it is committed: the headers and the
# static loader are downloaded here, at configure time, from the pinned
# github.com/NVIDIA/DLSS commit, and only builds that opt in link it. The
# DLSS model itself (nvngx_dlss.dll) is never built in or shipped; the
# provider loads the one the player puts in upscalers/dlss/<version>/.
#
# REXGLUE_DLSS_SDK_DIR may point at a local checkout of that repository
# instead of downloading.
#
# Sets REXGLUE_DLSS_INCLUDE_DIR and REXGLUE_DLSS_LIBRARY when available.

option(REXGLUE_DLSS "Build the DLSS provider (downloads NVIDIA's NGX loader)" OFF)
set(REXGLUE_DLSS_SDK_DIR "" CACHE PATH "Local checkout of github.com/NVIDIA/DLSS")

set(REXGLUE_DLSS_INCLUDE_DIR "" CACHE INTERNAL "")
set(REXGLUE_DLSS_LIBRARY "" CACHE INTERNAL "")
if(NOT REXGLUE_DLSS OR NOT REXGLUE_USE_VULKAN OR NOT WIN32 OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()

# DLSS 310.9.16.
set(_rexglue_dlss_commit 9981c6350dc58c0ce21f024fd65e31e851c31689)
set(_rexglue_dlss_files
    "include/nvsdk_ngx_defs.h|a9481cbc78d53ee56639f0a573ec095c6c1ea1e5f296d139c20c42bf342c2e1b"
    "include/nvsdk_ngx_defs_vk.h|43b821c5d297d86165e6d2f96a476f5c30f2158c7d5a0451425c6b927abfec88"
    "include/nvsdk_ngx_params.h|51a1f94116cf82f95c63a2c197371ed22e08bddb09918d36cc6e2ac712753c80"
    "include/nvsdk_ngx_vk.h|3be54d0103cce02174fd34912c60c9f40c73b5eec919e42613a2268c589c53ad"
    "include/nvsdk_ngx_helpers_vk.h|754697d9faa93913985fc91fceae2073763461710d742a1f262ced2a7b6b62c9"
    "lib/Windows_x86_64/x64/nvsdk_ngx_d.lib|bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c")

if(REXGLUE_DLSS_SDK_DIR)
    set(_rexglue_dlss_dir "${REXGLUE_DLSS_SDK_DIR}")
else()
    set(_rexglue_dlss_dir "${CMAKE_BINARY_DIR}/nvidia-dlss")
    set(_rexglue_dlss_url
        "https://raw.githubusercontent.com/NVIDIA/DLSS/${_rexglue_dlss_commit}")
endif()
foreach(_entry IN LISTS _rexglue_dlss_files)
    string(REPLACE "|" ";" _entry "${_entry}")
    list(GET _entry 0 _file)
    list(GET _entry 1 _hash)
    set(_path "${_rexglue_dlss_dir}/${_file}")
    if(EXISTS "${_path}")
        file(SHA256 "${_path}" _actual)
        if(_actual STREQUAL _hash)
            continue()
        endif()
        if(REXGLUE_DLSS_SDK_DIR)
            message(WARNING "${_path} is not the pinned DLSS 310.9.16 file; "
                "the DLSS provider is left out.")
            return()
        endif()
    elseif(REXGLUE_DLSS_SDK_DIR)
        message(WARNING "${_path} is missing; the DLSS provider is left out.")
        return()
    endif()
    file(DOWNLOAD "${_rexglue_dlss_url}/${_file}" "${_path}"
        EXPECTED_HASH SHA256=${_hash} STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_path}")
        message(WARNING "Could not download ${_file} from NVIDIA/DLSS (${_status}); "
            "the DLSS provider is left out.")
        return()
    endif()
endforeach()
set(REXGLUE_DLSS_INCLUDE_DIR "${_rexglue_dlss_dir}/include" CACHE INTERNAL "")
set(REXGLUE_DLSS_LIBRARY "${_rexglue_dlss_dir}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
    CACHE INTERNAL "")
