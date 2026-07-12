# Copyright 2026, DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# displayxr-cef-host (#625, Step A): a CEF offscreen-render browser stand-in
# that drives the REAL display-processor weave through the shipped XR_DXR_weave
# RPC. The host never weaves — it hands the runtime a pre-weave SBS sub-rect and
# composites the weaved handback (ADR-007 / ADR-019).
#
# Deps are fetched by scripts/setup-deps.bat to short, space-free paths:
#   CEF_ROOT     (default C:\dev\cef\149.0.4)        — CEF binary distribution
#   OpenXR_ROOT  (default C:\dev\openxr_sdk_1.1.43)  — OpenXR loader (.lib/.dll)
# The DisplayXR OpenXR extension headers (XR_DXR_weave etc.) are vendored under
# third_party/ (synced from the runtime's src/external/openxr_includes).

cmake_minimum_required(VERSION 3.21)

# Use the modern MSVC-runtime abstraction so the FetchContent'd displayxr-common
# honors our CRT choice. CEF defaults to the STATIC runtime (/MT), so we select
# MultiThreaded everywhere to match — mixing /MT and /MD across CEF, the wrapper,
# and displayxr-common would LNK2038.
cmake_policy(SET CMP0091 NEW)

project(displayxr_cef_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# CEF's FindCEF needs the target architecture.
if(NOT DEFINED PROJECT_ARCH)
    set(PROJECT_ARCH "x86_64")
endif()

# ---------------------------------------------------------------------------
# Vendored DisplayXR OpenXR headers (core + custom extensions). Used as the
# single header source for OpenXR so XR_DXR_weave is guaranteed present.
# ---------------------------------------------------------------------------
set(DXR_OPENXR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/displayxr_openxr_includes")
if(NOT EXISTS "${DXR_OPENXR_INCLUDE}/openxr/XR_DXR_weave.h")
    message(FATAL_ERROR "Vendored XR_DXR_weave.h missing under ${DXR_OPENXR_INCLUDE}")
endif()

# ---------------------------------------------------------------------------
# OpenXR loader (binary only; headers come from the vendored set above).
# Mirrors the runtime test apps' resolution: -DOpenXR_ROOT points at the
# short-path SDK copy; synthesize an IMPORTED SHARED OpenXR::openxr_loader.
# ---------------------------------------------------------------------------
if(NOT TARGET OpenXR::openxr_loader)
    if(NOT DEFINED OpenXR_ROOT)
        set(OpenXR_ROOT "C:/dev/openxr_sdk_1.1.43")
    endif()
    find_library(OPENXR_LOADER_LIB openxr_loader PATHS "${OpenXR_ROOT}/x64/lib" NO_DEFAULT_PATH)
    find_file(OPENXR_LOADER_DLL openxr_loader.dll PATHS "${OpenXR_ROOT}/x64/bin" NO_DEFAULT_PATH)
    if(NOT OPENXR_LOADER_LIB OR NOT OPENXR_LOADER_DLL)
        message(FATAL_ERROR "OpenXR loader not found under ${OpenXR_ROOT}. Run scripts/setup-deps.bat.")
    endif()
    add_library(OpenXR::openxr_loader SHARED IMPORTED)
    set_target_properties(OpenXR::openxr_loader PROPERTIES
        IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
        IMPORTED_LOCATION "${OPENXR_LOADER_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${DXR_OPENXR_INCLUDE}")
endif()

# ---------------------------------------------------------------------------
# displayxr-common (XrSessionManager, PollEvents, logging, Kooima math).
# Reuses our OpenXR::openxr_loader target; consumes the vendored EXT headers.
# Inherits CMAKE_MSVC_RUNTIME_LIBRARY (set above) -> /MT.
# ---------------------------------------------------------------------------
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${DXR_OPENXR_INCLUDE}" CACHE PATH "DisplayXR OpenXR extension headers")
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG 28d6eb7b9898cde447d90bfecda8b6734be83db1
)
FetchContent_MakeAvailable(displayxr_common)

# ---------------------------------------------------------------------------
# CEF binary distribution.
# ---------------------------------------------------------------------------
if(NOT DEFINED CEF_ROOT)
    set(CEF_ROOT "C:/dev/cef/149.0.4")
endif()
if(NOT EXISTS "${CEF_ROOT}/libcef_dll/CMakeLists.txt")
    message(FATAL_ERROR "CEF not found at ${CEF_ROOT}. Run scripts/setup-deps.bat.")
endif()
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")
# Disable the CEF sandbox: it forces the bootstrap-DLL exe layout (CEF_USE_BOOTSTRAP)
# and links the heavy static cef_sandbox.lib. A plain wWinMain exe is right for this
# dev harness (settings.no_sandbox = true at runtime to match).
set(USE_SANDBOX OFF CACHE BOOL "" FORCE)
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

# Logical target for libcef + per-target output dir (CEF macros).
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
SET_CEF_TARGET_OUT_DIR()

# ---------------------------------------------------------------------------
# The host executable.
# ---------------------------------------------------------------------------
set(HOST_SRCS
    src/main.cpp
    src/xr_session.cpp
    src/xr_session.h
    src/host_bridge.h
    src/cef_app.cpp
    src/cef_app.h
    src/cef_client.cpp
    src/cef_client.h
    src/weave_compositor.cpp
    src/weave_compositor.h
)

add_executable(displayxr_cef_host WIN32 ${HOST_SRCS})
SET_EXECUTABLE_TARGET_PROPERTIES(displayxr_cef_host)
add_dependencies(displayxr_cef_host libcef_dll_wrapper)

# CEF applies /W4 /WX to consumer targets; the FetchContent'd displayxr-common
# headers re-#define UNICODE / WIN32_LEAN_AND_MEAN (already on CEF's command
# line) -> benign C4005 that /WX turns fatal. Don't treat warnings as errors in
# our own translation units.
if(MSVC)
    target_compile_options(displayxr_cef_host PRIVATE /WX-)
endif()

target_include_directories(displayxr_cef_host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${DXR_OPENXR_INCLUDE}"
    "${CEF_ROOT}")

target_link_libraries(displayxr_cef_host PRIVATE
    libcef_lib
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS}
    displayxr::common
    OpenXR::openxr_loader
    d3d11 dxgi d3dcompiler)

# Embed our DPI / asInvoker manifest (relative path dodges spaces-in-path).
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/host.manifest"
               "${CMAKE_CURRENT_BINARY_DIR}/host.manifest" COPYONLY)
target_link_options(displayxr_cef_host PRIVATE
    "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/host.manifest")

# Place the exe in the CEF target output dir so the payload sits alongside it.
set_target_properties(displayxr_cef_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CEF_TARGET_OUT_DIR}")

# Copy the CEF runtime payload (libcef.dll, chrome_elf.dll, *.pak, icudtl.dat,
# v8 snapshots, locales/) next to the exe.
COPY_FILES(displayxr_cef_host "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES(displayxr_cef_host "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

# Copy the OpenXR loader DLL next to the exe.
add_custom_command(TARGET displayxr_cef_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PROPERTY:OpenXR::openxr_loader,IMPORTED_LOCATION>"
        "${CEF_TARGET_OUT_DIR}/openxr_loader.dll"
    VERBATIM)

# Copy the demo web page next to the exe (main.cpp loads ./web/index.html).
add_custom_command(TARGET displayxr_cef_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/web"
        "${CEF_TARGET_OUT_DIR}/web"
    VERBATIM)

PRINT_CEF_CONFIG()
