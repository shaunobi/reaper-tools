# Cross-compiles the Windows x64 build from Linux with clang targeting
# MinGW-w64 and the LLVM linker (lld). Used by the windows-mingw presets.
#
# This is a developer build for the local validation loop (fake host under
# Wine, debugging with gdb/winedbg). The shipped Windows extension still
# comes from CI with the Microsoft toolchain; see docs/BUILDING.md.
#
# Needs: clang, lld, g++-mingw-w64-x86-64 (for the MinGW headers, libraries
# and windres), and wine to run the results.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_srt_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${_srt_triple})
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET ${_srt_triple})
set(CMAKE_RC_COMPILER ${_srt_triple}-windres)

# lld understands the Microsoft import libraries ONNX Runtime ships and
# supports delay-loading; the default MinGW linker does neither well.
# The C++ runtime is linked statically so the DLL and test programs need
# nothing but Windows itself.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld -static")

set(CMAKE_FIND_ROOT_PATH /usr/${_srt_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Lets ctest run the Windows programs through Wine.
find_program(_srt_wine wine)
if(_srt_wine)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${_srt_wine}")
endif()
