#!/usr/bin/env bash
# Syntax-checks every source file against the Windows headers using the
# MinGW cross compiler, from a Linux machine. It does not produce a usable
# extension (REAPER needs the Microsoft toolchain's ABI); it exists to catch
# Windows-only mistakes such as a missing system header before CI does.
#
# Needs: g++-mingw-w64-x86-64 (apt), a configured linux-release build (for the
# generated headers and fetched dependencies), and network access the first
# time to fetch the DirectML provider header.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${MINGW_CXX:-x86_64-w64-mingw32-g++}
WINDRES=${MINGW_WINDRES:-x86_64-w64-mingw32-windres}
BUILD=${SRT_BUILD_DIR:-build/linux-release}
ORT_VERSION=$(grep -oP 'SRT_ORT_VERSION "\K[0-9.]+' cmake/OnnxRuntime.cmake)
DML_VERSION=$(grep -oP 'SRT_DIRECTML_VERSION "\K[0-9.]+' cmake/OnnxRuntime.cmake)
ORT_LINUX="$BUILD/_deps/onnxruntime/linux-x64/onnxruntime-linux-x64-$ORT_VERSION/include"
JSON_INC="$BUILD/_deps/nlohmann_json-src/include"
GEN_INC="$BUILD/src/core/generated"
CHECK_DIR="$BUILD/windows-check"

if ! command -v "$CXX" > /dev/null; then echo "missing $CXX (apt install g++-mingw-w64-x86-64)"; exit 2; fi
for d in "$ORT_LINUX" "$JSON_INC" "$GEN_INC"; do
  [ -d "$d" ] || { echo "missing $d: configure and build the linux-release preset first"; exit 2; }
done

# The DirectML provider header only ships in the Windows NuGet package.
DML_INC="$CHECK_DIR/dml-include"
if [ ! -f "$DML_INC/dml_provider_factory.h" ]; then
  mkdir -p "$CHECK_DIR"
  echo "fetching the DirectML provider header (once)"
  curl -sSL -o "$CHECK_DIR/ort-dml.zip" "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/$ORT_VERSION"
  mkdir -p "$DML_INC"
  unzip -qo -j "$CHECK_DIR/ort-dml.zip" "build/native/include/dml_provider_factory.h" -d "$DML_INC"
  # ... and DirectML.h itself, which that header includes.
  curl -sSL -o "$CHECK_DIR/directml.zip" "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/$DML_VERSION"
  unzip -qo -j "$CHECK_DIR/directml.zip" "include/DirectML.h" -d "$DML_INC"
fi
# DirectML.h uses Microsoft source annotations that MinGW's sal.h lacks;
# discover the missing ones and define them as empty.
STUBS="$DML_INC/sal_stubs.h"
if [ ! -f "$STUBS" ]; then
  : > "$STUBS"
  printf '#include <windows.h>\n#include <DirectML.h>\n' > "$CHECK_DIR/dmltest.cpp"
  for _ in 1 2 3 4 5 6; do
    missing=$("$CXX" -std=c++17 -fsyntax-only -Wno-unknown-pragmas -I"$DML_INC" -include "$STUBS" "$CHECK_DIR/dmltest.cpp" 2>&1 \
      | grep -oP "'\K_[A-Za-z_]+(?=' (does not name a type|was not declared))" | sort -u || true)
    [ -z "$missing" ] && break
    for m in $missing; do printf '#ifndef %s\n#define %s\n#endif\n' "$m" "$m"; done >> "$STUBS"
  done
fi

FLAGS=(-std=c++17 -fsyntax-only -Wall -Wextra -Wno-unknown-pragmas
  -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DSRT_HAVE_DML=1
  -include "$DML_INC/sal_stubs.h"
  -Isrc/core/include -I"$GEN_INC" -I"$ORT_LINUX" -I"$DML_INC" -I"$JSON_INC"
  -isystem third_party/pocketfft -isystem third_party/r8brain
  -isystem third_party/reaper-sdk/sdk -isystem third_party/WDL/WDL -Isrc/reaper)

status=0
files=$(ls src/core/src/*.cpp src/reaper/*.cpp src/cli/main.cpp | grep -v -E "download_curl|download_stub")
for f in $files; do
  if ! "$CXX" "${FLAGS[@]}" "$f" 2> "$CHECK_DIR/err.txt"; then
    echo "FAIL $f"; sed 's/^/    /' "$CHECK_DIR/err.txt" | head -20; status=1
  fi
done
if command -v "$WINDRES" > /dev/null; then
  if ! "$WINDRES" -I src/reaper -i src/reaper/resource.rc -o "$CHECK_DIR/resource.o" 2> "$CHECK_DIR/err.txt"; then
    echo "FAIL src/reaper/resource.rc"; sed 's/^/    /' "$CHECK_DIR/err.txt" | head -20; status=1
  fi
fi
[ $status -eq 0 ] && echo "windows syntax check: all files OK"
exit $status
