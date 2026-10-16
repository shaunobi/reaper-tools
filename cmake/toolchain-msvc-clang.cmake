# Cross-compiles the Windows x64 build from Linux with clang in its
# Microsoft-compatible mode (clang-cl) and the LLVM linker (lld-link),
# against the Windows SDK and Visual C++ runtime headers and libraries that
# the `xwin` tool downloads from Microsoft. Used by the windows-clang presets.
#
# The result uses the same C++ object layout, exception handling and C
# runtime as a Visual Studio build, so the DLL can be loaded by REAPER and
# can talk to REAPER's C++ interfaces (PCM_source and friends). The MinGW
# toolchain (toolchain-mingw-clang.cmake) cannot promise that. See
# docs/BUILDING.md, "Building the Windows extension from Linux".
#
# Setup (once):  xwin --accept-license --arch x86_64 splat --output ~/.cache/xwin --preserve-ms-arch-notation
# Needs: clang-cl, lld-link, llvm-rc, llvm-mt (LLVM 16 or newer), wine to run the results.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(SRT_XWIN_DIR "$ENV{HOME}/.cache/xwin" CACHE PATH "Folder xwin splat wrote (crt/ and sdk/)")
if(NOT EXISTS "${SRT_XWIN_DIR}/crt/include/vcruntime.h")
  message(FATAL_ERROR "No Windows SDK/CRT at ${SRT_XWIN_DIR}. Run: xwin --accept-license --arch x86_64 splat --output ${SRT_XWIN_DIR} --preserve-ms-arch-notation")
endif()
# xwin names the architecture folder x64 (with --preserve-ms-arch-notation) or x86_64.
set(_srt_arch x64)
if(NOT EXISTS "${SRT_XWIN_DIR}/crt/lib/${_srt_arch}")
  set(_srt_arch x86_64)
endif()

find_program(SRT_CLANG_CL NAMES clang-cl clang-cl-18 clang-cl-19 clang-cl-20 REQUIRED)
find_program(SRT_LLD_LINK NAMES lld-link lld-link-18 lld-link-19 lld-link-20 REQUIRED)
find_program(SRT_LLVM_RC NAMES llvm-rc llvm-rc-18 llvm-rc-19 llvm-rc-20 REQUIRED)
find_program(SRT_LLVM_MT NAMES llvm-mt llvm-mt-18 llvm-mt-19 llvm-mt-20)
find_program(SRT_LLVM_LIB NAMES llvm-lib llvm-lib-18 llvm-lib-19 llvm-lib-20)

set(CMAKE_C_COMPILER "${SRT_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${SRT_CLANG_CL}")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER "${SRT_LLD_LINK}")
set(CMAKE_RC_COMPILER "${SRT_LLVM_RC}")
if(SRT_LLVM_MT)
  set(CMAKE_MT "${SRT_LLVM_MT}")
endif()
if(SRT_LLVM_LIB)
  set(CMAKE_AR "${SRT_LLVM_LIB}")
endif()

# System include folders (/imsvc = "system include, Microsoft style": no warnings from them).
set(_srt_inc "")
foreach(_d crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt sdk/include/cppwinrt)
  string(APPEND _srt_inc " /imsvc\"${SRT_XWIN_DIR}/${_d}\"")
endforeach()
# Microsoft's C++ library checks for the clang version Visual Studio was
# tested with; the define below is its documented way to build with another.
set(CMAKE_C_FLAGS_INIT "${_srt_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_srt_inc} /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
set(CMAKE_RC_FLAGS_INIT "/I\"${SRT_XWIN_DIR}/sdk/include/um\" /I\"${SRT_XWIN_DIR}/sdk/include/shared\" /I\"${SRT_XWIN_DIR}/crt/include\"")

set(_srt_lib "/libpath:\"${SRT_XWIN_DIR}/crt/lib/${_srt_arch}\" /libpath:\"${SRT_XWIN_DIR}/sdk/lib/um/${_srt_arch}\" /libpath:\"${SRT_XWIN_DIR}/sdk/lib/ucrt/${_srt_arch}\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_srt_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_srt_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_srt_lib}")

# xwin fetches only the release C runtime libraries, so debug builds link
# the release runtime too (the DLL still gets full debug information).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)

# Lets ctest run the Windows programs through Wine.
find_program(_srt_wine wine)
if(_srt_wine)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${_srt_wine}")
endif()
