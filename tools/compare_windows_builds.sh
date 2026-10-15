#!/usr/bin/env bash
# Compares two Windows builds of the extension: what each DLL links against,
# and how each behaves when the fake host loads it under Wine and runs the
# actions. Typical use: the DLL GitHub Actions built (Microsoft toolchain)
# against the local cross-build (clang + MinGW), or two CI builds.
#
#   tools/compare_windows_builds.sh [--old-runtime <onnxruntime.dll>] <dirA> <dirB> [scenario ...]
#
# Each dir holds reaper_srt.dll with onnxruntime.dll and DirectML.dll next to
# it (a CI artifact folder, or build/windows-mingw/src/reaper). Scenarios
# default to "about fit conform"; "separate" is slow under Wine.
# --old-runtime copies the given (older) onnxruntime.dll into the Wine
# prefix's System32, mimicking a Windows machine that ships one, which is
# the situation the delay-load code in runtime_loader.cpp exists for.
#
# Needs: the windows-clang preset built (for srt_fakehost.exe; the
# windows-mingw one is used as a fallback but cannot drive a Microsoft-built
# DLL through REAPER's C++ interfaces), wine, xvfb-run, and the test models
# the configure step downloads into build/<preset>/_models.
set -euo pipefail
cd "$(dirname "$0")/.."
OLD_RUNTIME=""
if [ "${1:-}" = "--old-runtime" ]; then OLD_RUNTIME=$(realpath "$2"); shift 2; fi
[ $# -ge 2 ] || { sed -n 2,20p "$0"; exit 2; }
A=$(realpath "$1"); B=$(realpath "$2"); shift 2
SCENARIOS=${*:-about fit conform}
HOST=${SRT_FAKEHOST:-build/windows-clang/src/fakehost/srt_fakehost.exe}
if [ ! -f "$HOST" ] && [ -f build/windows-mingw/src/fakehost/srt_fakehost.exe ]; then
  HOST=build/windows-mingw/src/fakehost/srt_fakehost.exe
  echo "note: using the MinGW fake host; a Microsoft-built DLL will fail past the About scenario (C++ layout mismatch)"
fi
[ -f "$HOST" ] || { echo "build the windows-clang preset first (missing $HOST)"; exit 2; }
for d in "$A" "$B"; do [ -f "$d/reaper_srt.dll" ] || { echo "no reaper_srt.dll in $d"; exit 2; }; done
OBJDUMP=${OBJDUMP:-x86_64-w64-mingw32-objdump}
OUT=build/compare; mkdir -p "$OUT"

export WINEDEBUG=-all
export WINEPREFIX=$PWD/build/wine-prefix-compare
[ -d "$WINEPREFIX" ] || wineboot --init > /dev/null 2>&1
SYS32="$WINEPREFIX/drive_c/windows/system32"
rm -f "$SYS32/onnxruntime.dll"
if [ -n "$OLD_RUNTIME" ]; then
  cp "$OLD_RUNTIME" "$SYS32/onnxruntime.dll"
  echo "planted $OLD_RUNTIME as System32\\onnxruntime.dll"
fi

echo "================ what each DLL links against"
describe() {
  local f=$1
  echo "--- $f ($(stat -c %s "$f") bytes)"
  echo "imports:      $($OBJDUMP -p "$f" | grep 'DLL Name' | sed 's/.*DLL Name: //' | tr '\n' ' ')"
  if $OBJDUMP -p "$f" | grep -E '^Entry +d ' | grep -qE ' 0*[1-9a-f][0-9a-f]* +[0-9a-f]+ Delay'; then
    echo "delay-loaded: yes (onnxruntime.dll is bound at first use, see runtime_loader.cpp)"
  else
    echo "delay-loaded: NO - onnxruntime.dll is resolved by Windows at load time"
  fi
  echo "exports:      $($OBJDUMP -p "$f" | awk '/Export Address Table/{f=1} /PE File Base Relocations|The Import Tables/{f=0} f' | grep -E '^\s+\[\s*[0-9]+\] [A-Za-z_]' | sed -E 's/.*\] //' | tr '\n' ' ')"
  if $OBJDUMP -p "$f" | grep -q 'VCRUNTIME140'; then echo "C++ runtime:  Microsoft (needs the Visual C++ redistributable installed)"
  elif $OBJDUMP -p "$f" | grep -qi 'libstdc++'; then echo "C++ runtime:  MinGW, dynamic (needs libstdc++-6.dll next to it)"
  else echo "C++ runtime:  linked in (no extra files needed)"; fi
}
describe "$A/reaper_srt.dll"
describe "$B/reaper_srt.dll"

echo
echo "================ behaviour under Wine (fake host, --plugin)"
CLIP='Z:'$(realpath tests/data/beat/clip.wav | sed 's#/#\\#g')
MIX='Z:'$(realpath tests/data/sep/mix.wav | sed 's#/#\\#g')
args() {
  case $1 in
    about) echo "about";;
    fit) echo "fit $CLIP --bpm 120 --start-bar -2 --expect-bars 4";;
    conform) echo "conform $CLIP --bpm 120 --start-bar -2 --expect-bars 4";;
    separate) echo "separate $MIX";;
    *) echo "$1";;
  esac
}
# Keeps the console lines, drops paths, addresses and timings that differ run to run.
normalise() { grep -E '^(SRT:|  ok |  FAIL|RESULT)' "$1" | grep -v 'jobs window' | sed -E 's/Z:\\[^ ]*//g; s/0x[0-9A-Fa-f]+//g; s/[0-9]+\.[0-9]+ s\b/N s/g; s/tid\([0-9a-f]+\)//g'; }
status=0
for sc in $SCENARIOS; do
  for side in A B; do
    dir=$([ $side = A ] && echo "$A" || echo "$B")
    log="$OUT/$side-$sc.log"
    # shellcheck disable=SC2046
    (cd "$(dirname "$HOST")" && timeout 1800 xvfb-run -a wine ./srt_fakehost.exe --plugin "Z:$(echo "$dir" | sed 's#/#\\#g')\\reaper_srt.dll" $(args "$sc") > "$OLDPWD/$log" 2>&1) || true
    res=$(grep -E '^RESULT' "$log" || echo "RESULT NONE (crashed or hung; see $log)")
    printf '%-8s %s: %s\n' "$sc" "$side" "$res"
    echo "$res" | grep -q PASS || status=1
  done
  if ! diff <(normalise "$OUT/A-$sc.log") <(normalise "$OUT/B-$sc.log") > "$OUT/$sc.diff"; then
    echo "         console output differs ($OUT/$sc.diff):"
    sed 's/^/           /' "$OUT/$sc.diff" | head -40
  else
    echo "         console output identical"
  fi
done
echo
echo "A = $A"; echo "B = $B"; echo "logs in $OUT/"
exit $status
