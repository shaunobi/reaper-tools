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
src/reaper   reaper_srt: the REAPER extension
tests        Catch2 unit tests
tools        developer-side scripts (Python) for models
third_party  git submodules: reaper-sdk, WDL, pocketfft
cmake        dependency fetching and helpers
```

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
