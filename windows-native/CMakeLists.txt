cmake_minimum_required(VERSION 3.21)
project(TunHub VERSION 0.9.0 LANGUAGES CXX)

# Native Win32 build: no .NET, no WindowsAppSDK. The previous WinUI 3 build shipped two
# self-contained .NET runtimes plus the WindowsAppSDK native libraries (~175 MB of the
# ~200 MB installer). Here the app and the service are plain Win32 binaries of a few MB,
# and the bulk of the installer is the tunnel cores themselves (Go/OpenVPN).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "TunHub for Windows is built with MSVC (Visual Studio 2022 toolset).")
endif()

# Static CRT: no vcruntime/msvcp redistributable next to the binaries, and nothing to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(
  /W4 /permissive- /utf-8 /EHsc
  /Zc:__cplusplus
  $<$<CONFIG:Release>:/O2>
  $<$<CONFIG:Release>:/GL>          # whole-program optimisation (smaller, faster)
)
add_link_options(
  $<$<CONFIG:Release>:/LTCG>
  $<$<CONFIG:Release>:/OPT:REF>     # drop unreferenced code
  $<$<CONFIG:Release>:/OPT:ICF>     # fold identical code
)

add_compile_definitions(
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  TUNHUB_VERSION="${PROJECT_VERSION}"
)

add_subdirectory(src/core)      # models, parsers, JSON — no Windows UI dependencies

# Added as each layer lands; see windows-native/PORTING.md for the running status.
foreach(layer engine helper app)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${layer}/CMakeLists.txt")
    add_subdirectory(src/${layer})
  endif()
endforeach()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  enable_testing()
  add_subdirectory(tests)
endif()
