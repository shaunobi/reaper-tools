# Building SRT

## Prerequisites

- CMake 3.24 or newer and Ninja (Windows uses Visual Studio's generator instead).
- Windows: Visual Studio 2022 with the "Desktop development with C++" workload.
- macOS: Xcode command-line tools. Apple Silicon only for now.
- Linux: GCC 13 or Clang 18. Linux builds are for compile checks and tests;
  the extension itself is not tested against REAPER for Linux yet.
- Perl (macOS and Linux only) for converting the dialog resource to SWELL form.
  macOS ships it; on Linux install the `perl` package.
- Linux: `libcurl4-openssl-dev` for model downloads (optional; macOS and
  Windows use system libraries).

Network access during the first configure: CMake downloads the prebuilt ONNX
Runtime for your platform (and DirectML on Windows) into the build folder.

## Get the source

```
git clone --recurse-submodules https://github.com/shaunobi/reaper-tools.git
cd reaper-tools
```

If you already cloned without submodules: `git submodule update --init --recursive`.

## Build and test

```
cmake --preset windows-release      # or macos-release, linux-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## Install into REAPER

```
cmake --install build/windows-release --config Release --prefix dist
```

That produces `dist/UserPlugins/` containing the extension and the ONNX
Runtime libraries it needs. Copy the contents into REAPER's `UserPlugins`
folder (REAPER > Options > Show REAPER resource path), then restart REAPER.
The action list should show "SRT: About (version, runtime check)". Running
it prints version and provider information to the ReaScript console and
runs a small model on the best available execution provider.

## Layout

```
src/core     srt_core: audio processing, no REAPER dependency
src/cli      srt_cli: command-line harness over srt_core
src/reaper   reaper_srt: the REAPER extension (its logic is a static library
             the fake host links too)
src/fakehost srt_fakehost: a stand-in for REAPER that runs the real actions
tests        Catch2 unit tests
tools        developer-side scripts (Python) for models
third_party  git submodules: reaper-sdk, WDL, pocketfft
cmake        dependency fetching and helpers
```

## Running the actions without REAPER (the fake host)

`srt_fakehost` is a small program that stands in for REAPER. It holds a
project in memory (tracks, items, takes, stretch markers, a constant tempo
map, the extension-state strings), answers the same API calls the extension
makes, and loads the extension through the real entry point. It then runs an
action by its command ID, pumps the main-thread timer until the background
job finishes, prints the project state, and checks the result. Dialogs are
answered automatically (the model-download question gets "yes"); the jobs
window is replaced by progress lines on standard output.

```
cmake --preset linux-release      # or macos-release / windows-release
cmake --build --preset linux-release
ctest --preset linux-release -R fakehost
```

The five `fakehost.*` tests cover About, Fit drums to grid (with and without
lead-in bars), Conform drum bars, and Separate stems, each against the real
model and the same test clips the unit tests use. They are skipped if the
models have not been downloaded into `build/<preset>/_models` (the configure
step does that unless `SRT_FETCH_TEST_MODELS` is off).

To run one scenario by hand, from the folder that holds the program
(`build/linux-release/src/fakehost`):

```
./srt_fakehost about
./srt_fakehost fit ../../../../tests/data/beat/clip.wav --bpm 120 --start-bar -2 --expect-bars 4
./srt_fakehost conform my_drums.wav --bpm 118 --start-bar 1 --position 2.5
./srt_fakehost separate my_mix.wav
./srt_fakehost shell my_drums.wav     # type command IDs, "dump", "bpm 121", "startbar -2", "quit"
```

`--start-bar` is REAPER's "project start measure" setting: `-2` means bars
-2, -1 and 0 come before bar 1 (so bar 1 is the target), `1` means the
project starts at bar 1 (so bar 2 is the target). `--position` is where the
item sits before the action runs. Exit code 0 means every check passed, 1
means a check failed, 77 means the model was missing.

Work directory: `build/<preset>/fakehost/` holds the fake project's media
folder (where separated stems land) and a resource folder standing in for
REAPER's, with the models linked in under `SRT/models/`.

### Debugging with symbols

The `linux-debug` preset builds everything with debug information and no
optimisation. Run a scenario under gdb (or lldb) and break anywhere in the
extension:

```
cmake --preset linux-debug && cmake --build --preset linux-debug
cd build/linux-debug/src/fakehost
gdb --args ./srt_fakehost fit ../../../../tests/data/beat/clip.wav --bpm 120 --start-bar -2
(gdb) break srt::reaper::actionFitDrumsToGrid
(gdb) run
```

The same works on macOS with lldb, and on Windows by opening the
`srt_fakehost` target in Visual Studio (`build/windows-release` is a Visual
Studio solution; switch the configuration to Debug).

What the fake host does not cover: the real dialogs and the jobs window
(SWELL/Win32 code in `progress_window.cpp`), REAPER's own stretch-marker
playback, tempo maps with tempo changes, and anything about how REAPER loads
the library. A crash on load inside REAPER still needs REAPER.

## Checking Windows-only code from Linux

REAPER extensions for Windows must be built with Microsoft's compiler, but a
Linux machine can still catch Windows-only mistakes (a missing system header,
a non-portable call) before CI does:

```
sudo apt install g++-mingw-w64-x86-64
tools/check_windows_syntax.sh
```

This syntax-checks every source file and the dialog resource against the
Windows headers with the MinGW cross compiler. It takes about a minute and
needs a configured `linux-release` build. It is a check, not a build: the
shipped Windows extension always comes from CI or a Visual Studio build.

## Trying the beat tracker from the command line

The tests download the Beat This! model into `build/<preset>/_models/`. Then:

```
build/linux-release/src/cli/srt_cli beats my-drums.wav --model build/linux-release/_models/beat-this-final0.onnx
```

prints the execution provider used, the fitted tempo, and one line per bar
with its downbeat time. Add `--csv` for one line per beat with a downbeat
flag, or `--no-refine` to see the raw 20 ms model positions without onset
refinement.

## Trying separation from the command line

```
build/linux-release/src/cli/srt_cli separate song.wav --model build/linux-release/_models/bs_roformer_4stem_rhythm_fp16.onnx
```

writes `song - drums.wav`, `song - bass.wav`, `song - other.wav` and
`song - vocals.wav` next to the input. On a CPU each 8-second chunk takes
tens of seconds; on a graphics card about a second.

`srt_cli download <url> <dest> --sha256 <hex>` and `srt_cli sha256 <file>`
exercise the model download path. Linux builds need `libcurl4-openssl-dev`
for downloads; without it the extension still builds but reports that it
cannot download.

## Regenerating the reference data for the beat tracker tests

```
.venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu torch torchaudio
.venv/bin/pip install beat_this onnxruntime numpy
.venv/bin/python tools/make_beat_reference.py build/linux-release/_models/beat-this-final0.onnx
```

## Regenerating the smoke model

```
python -m venv .venv && .venv/bin/pip install onnx numpy
.venv/bin/python tools/make_smoke_model.py
```

## Releasing

```
echo 0.1.0 > version.txt && git commit -am "Version 0.1.0"
git tag -a v0.1.0 -m "First internal release: About, Fit drums to grid, Conform drum bars, Separate stems"
git push origin main v0.1.0
```

CI builds both platforms, attaches the files to a draft GitHub release, and
adds the version to `index.xml` on `main`. Publish the draft release, then
anyone can import
`https://raw.githubusercontent.com/shaunobi/reaper-tools/main/index.xml`
into ReaPack (Extensions > ReaPack > Import repositories).
