cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" SRT_VERSION_RAW)
string(STRIP "${SRT_VERSION_RAW}" SRT_VERSION)

project(srt VERSION ${SRT_VERSION} LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SRT_BUILD_TESTS "Build unit tests" ON)
option(SRT_BUILD_CLI "Build the srt_cli command-line harness" ON)
option(SRT_BUILD_REAPER "Build the REAPER extension" ON)

# Where the finished extension and its runtime libraries are collected.
set(SRT_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Output folder for the packaged extension")

if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/MP) # parallel compilation; clang-cl does not take it
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(SRT_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(SRT_WDL_DIR "${SRT_THIRD_PARTY}/WDL/WDL")
set(SRT_REAPER_SDK_DIR "${SRT_THIRD_PARTY}/reaper-sdk/sdk")

if(NOT EXISTS "${SRT_REAPER_SDK_DIR}/reaper_plugin.h" OR NOT EXISTS "${SRT_WDL_DIR}/swell/swell.h")
  message(FATAL_ERROR "Submodules are missing. Run: git submodule update --init --recursive")
endif()

include(cmake/OnnxRuntime.cmake)
include(cmake/Dependencies.cmake)

if(SRT_BUILD_TESTS)
  enable_testing() # before the subdirectories, so their add_test() calls count
endif()

add_subdirectory(src/core)
if(SRT_BUILD_CLI)
  add_subdirectory(src/cli)
endif()
if(SRT_BUILD_REAPER)
  add_subdirectory(src/reaper)
  add_subdirectory(src/fakehost)
endif()
if(SRT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
