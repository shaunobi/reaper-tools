# SRT: Shaunobi's Reaper Tools — Specification

Version 0.1 (draft), 2026-09-18

SRT is a REAPER extension, written in C++, that adds studio-workflow actions
to REAPER's action list in the same way the SWS extension does. It is built
for the workflow Shaun and Dean use in the studio, and is shared publicly
through ReaPack.

This document records what we are building, the decisions already made, and
the questions still open. It is the reference for the first three actions.
Later actions get their own section here when they are agreed.

---

## 1. Goals and non-goals

### Goals

1. **Separate stems.** Take a selected item and split it into drums, bass,
   vocals and other, using an open-weight machine-learning model that runs
   on the local machine. Drum quality is the priority.
2. **Fit drums to grid.** Take a drum item that is not on the project grid
   (for example a stem from a song at 121 BPM in a 120 BPM project), find its
   first and last downbeat, work out how many bars it contains, and move and
   stretch it so bar 1 and the final downbeat sit exactly on the project's
   bar lines.
3. **Conform drum bars.** With the ends already fitted, put a stretch marker
   on every downbeat in between and snap each one to its bar line, removing
   the drift in the middle.
4. Everything runs inside REAPER, in the background, with a progress bar, and
   never blocks the user from working.
5. Correctness over convenience: model output must match the original
   research implementation, and detection must be state of the art.
6. Shareable with anyone through ReaPack, on Windows first and Apple Silicon
   second.

### Non-goals for the first release

- Multi-microphone drum recordings (phase-locked groups of tracks). One item
  at a time only.
- Splitting the drum stem further into kick, snare, toms, hats, cymbals.
- Ensembles of several separation models. One solid model.
- Tempo changes inside the item. The tempo map is assumed constant across
  the item. Support is planned later.
- Real-time or streaming use. Everything is offline processing.
- Time signatures other than 4/4.

---

## 2. Decisions log

Every entry here was agreed in conversation. Change them only on purpose.

| Topic | Decision |
|---|---|
| Language | C++17 throughout. No Python on the user's machine. |
| Form | REAPER extension (shared library loaded by REAPER at startup), like SWS. |
| Platforms | Windows x64 first. macOS Apple Silicon second. |
| Graphics card use | Windows: ONNX Runtime with the DirectML execution provider (works on Nvidia and AMD). macOS: CoreML execution provider. CPU fallback everywhere. Nvidia CUDA is not offered in the first release. |
| Machine-learning runtime | ONNX Runtime. Models are data files loaded at run time so new models never need a rebuild. |
| Separation model | Open BS-RoFormer 4-stem checkpoint (drums, bass, other, vocals), exported to ONNX with the STFT outside the graph. Loader is generic over stem count and model architecture. |
| Beat/downbeat model | "Beat This!" (Foscarin, Schlüter, Widmer, ISMIR 2024), final0 checkpoint, exported to ONNX with the spectrogram outside the graph. |
| Model delivery | Not in ReaPack. Downloaded on first use from the GitHub release of the matching version into REAPER's resource folder, with progress bar and checksum check. |
| Model conversion | Done on the developer side in Python at release time only. Every published model ships with a correctness test against the original PyTorch output. |
| Selection scope | Selected media items. With several selected, each is processed separately. |
| Stem output placement | New tracks directly below the source track, inside a folder, one track per stem, items at the same position and length as the source. Original item and track untouched. |
| Stem file format | 32-bit float WAV in the project media folder, at the source item's sample rate. |
| Resampling | REAPER's own resampler (extension SDK). Resample to the model rate on the way in, back to the source rate on the way out. |
| What is separated | The visible portion of the active take. Item volume, fades, take effects and pitch/rate settings are ignored; stems are made from the raw source audio. |
| Alignment target | The project tempo map, which is already correct when the actions run. The item is conformed to the map, never the other way around. |
| Alignment resolution | Whole bars (downbeats). Hard snap, no strength setting. |
| Tempo assumption | The recording's tempo is within about 15% of the project tempo at the item position. The tool uses that as a constraint. |
| First and last bar | A "first" and "last" stretch marker define bar 1 and the final downbeat. If the user has placed them, the tool trusts them. If not, the tool detects them. Snap offset is not used. |
| Where bar 1 lands | The first downbeat goes to project bar 1. Shaun's projects normally start at bar -2, so lead-in audio before bar 1 has room. If the project has no bars before bar 1, the first downbeat goes to bar 2 instead. |
| Last bar | The final downbeat that has a hit on it, not the end of the final bar. |
| Existing stretch markers | Ask before clearing, then clear. |
| Stretch algorithm | Whatever the item already has. Not changed. |
| Uncertain guesses | Always make the best guess. Report tempo, bar count and confidence to the REAPER console. |
| Undo | One undo step per action. |
| License | BSD 3-Clause. All dependencies are MIT, BSD or equivalent. No GPL. |
| Distribution | Git tags trigger a GitHub Actions build for both platforms. The ReaPack index lives in this repository. |
| Action naming | "SRT: Separate stems", "SRT: Fit drums to grid", "SRT: Conform drum bars". Command IDs `_SRT_SEPARATE_STEMS`, `_SRT_FIT_DRUMS_TO_GRID`, `_SRT_CONFORM_DRUM_BARS`. |
| Extension file | `reaper_srt.dll` (Windows), `reaper_srt.dylib` (macOS). |
| Order of work | Foundation first, then the two alignment actions, then separation. |

---

## 3. Architecture

Three layers, kept strictly apart so the audio work can be tested without
REAPER.

```
+---------------------------------------------------------------+
|  reaper_srt  (REAPER glue: actions, item/track edits, UI)      |
+---------------------------------------------------------------+
|  srt_core    (audio processing library, no REAPER dependency)  |
|    - audio buffers, WAV read/write, resampling interface       |
|    - STFT / mel spectrogram front ends                         |
|    - onnx runner (session, providers, chunking)                |
|    - separation pipeline                                       |
|    - beat/downbeat pipeline + onset refinement                 |
|    - bar fitting maths (pure functions)                        |
+---------------------------------------------------------------+
|  srt_cli     (command-line harness over srt_core for testing)  |
+---------------------------------------------------------------+
```

- **srt_core** is a static library. It knows nothing about REAPER. It takes
  audio in, gives audio or timestamps out. Every algorithm is testable from
  the command line on a WAV file.
- **srt_cli** is a small command-line tool that exposes srt_core: separate a
  file, detect beats in a file, print bar-fit results. This is how accuracy
  is measured in CI and how Shaun can try things outside REAPER.
- **reaper_srt** is the extension. It registers the actions, reads audio out
  of items, hands work to srt_core on a background thread, and applies the
  results (new tracks, stretch markers, item moves) on REAPER's main thread.

### Threading model

REAPER's API may only be called from its main thread. So:

1. Action fires on the main thread. It gathers everything it needs from the
   project (audio samples, positions, tempo) into a plain job description.
2. The job runs on a worker thread using srt_core only. It reports progress
   through an atomic counter and a cancel flag.
3. A REAPER timer callback (registered with `plugin_register("timer", ...)`)
   polls running jobs on the main thread, updates the progress window, and
   when a job finishes, applies its results inside one undo block.

The user can keep editing while a job runs. Edits to the source item during
a job are the user's risk; results are applied to the item by GUID and the
action reports if the item is gone.

### Progress window

A small non-modal window (WDL/SWELL, so it builds on both platforms) listing
running jobs, each with a name, a progress bar, and a cancel button. Opened
automatically when a job starts, closed automatically when the last job
finishes.

---

## 4. Action: SRT: Separate stems

**Command ID:** `_SRT_SEPARATE_STEMS`

### Input

All selected media items. Each is an independent job. Items with no audio
take (MIDI, empty) are skipped with a console message.

### Steps per item

1. Read the active take's source audio for the visible item range, honoring
   the take's start offset. Stereo in; mono is duplicated to two channels.
2. Resample to the model rate (44.1 kHz) with REAPER's resampler if needed.
3. Run the separation pipeline (section 6).
4. Resample each stem back to the source rate.
5. Write one 32-bit float WAV per stem into the project media folder:
   `<take name> - <stem>.wav`. If the name exists, append a number.
6. On the main thread, in one undo block:
   - Insert a folder track directly below the source track named
     `<take name> stems`.
   - Inside it, one track per stem in model order, named `<stem>`.
   - On each, one item at the source item's position and length, with the
     stem file as its source. Take name `<take name> - <stem>`.
   - Close the folder after the last stem track.
   - Leave the source item and track untouched.
7. Console line: `SRT: separated "<take name>" into 4 stems in 42.3 s (DirectML)`.

### Cancel

Cancel discards the job; no files are written and nothing changes in the
project.

### Errors

Missing model triggers a download prompt (section 8). Any failure is reported
to the console and the job ends without touching the project.

---

## 5. Actions: drum alignment

Two actions share one detection pipeline (section 7) and one convention for
marking bar 1 and the final downbeat.

### The first and last marker convention

A drum item is "fitted" when it carries exactly two stretch markers that SRT
recognises as its **first** marker (bar 1 downbeat) and **last** marker
(final downbeat). SRT stores their identity in the item's extension state
(`P_EXT:SRT_FIRST`, `P_EXT:SRT_LAST`, as source-position values) so they can
be found again after the user moves things.

The user may place these two markers by hand before running either action.
If exactly two stretch markers exist and no SRT state is present, SRT asks:
"Use the existing two stretch markers as first and last bar?" Yes adopts
them, No clears and detects.

### 5.1 SRT: Fit drums to grid

**Command ID:** `_SRT_FIT_DRUMS_TO_GRID`

Applies to each selected item independently.

1. **Existing markers.** If the item has stretch markers that are not the
   SRT first/last pair, ask before clearing them. Cancel aborts this item.
2. **Detect.** Run the detection pipeline on the item's audio. Output: list
   of downbeat times (in source seconds), tempo estimate, confidence.
   Skipped if first and last were supplied by the user, in which case the
   bar count comes from step 3 alone.
3. **Choose first and last.**
   - First = the first detected downbeat, unless supplied by the user.
   - Last = the last detected downbeat, unless supplied by the user.
   - Bar count `N` = number of detected downbeats from first to last
     inclusive, minus one. If the user supplied both markers, `N` is
     `round((last - first) / (60 / tempo_estimate * 4))` using the tempo
     estimate from detection.
4. **Target bars.** Read the project's measure offset. If the project has at
   least one bar before bar 1, the target for first is the start of bar 1.
   Otherwise the target is the start of bar 2. Target for last is the start
   of bar `(first bar + N)`.
5. **Move.** Set the item position so that the first marker's source time
   lands on the first target bar line. Lead-in audio before the first
   downbeat now sits before bar 1.
6. **Stretch.** Place stretch markers at first and last source positions.
   Set the last marker's project position so it lands on the last target
   bar line. REAPER stretches everything between them uniformly. Audio after
   the last marker keeps the same stretch ratio.
7. **Record.** Save first/last identity in extension state.
8. **Report.** Console line, for example:
   `SRT: "Drums" — 121.4 BPM detected, 64 bars, first downbeat at 0.512 s,
   fitted to bars 1–65, confidence high`.

Confidence is "high", "medium" or "low" from the detection pipeline. Low
confidence adds a second line explaining why (for example "tempo estimate
disagrees with project tempo by 22%").

### 5.2 SRT: Conform drum bars

**Command ID:** `_SRT_CONFORM_DRUM_BARS`

Applies to each selected item independently.

1. **Precondition.** The item must carry the SRT first/last pair (from the
   Fit action or adopted from user markers). If not, run the Fit action's
   steps 1 to 7 first, then continue. This makes Conform usable on its own.
2. **Existing markers.** Any stretch markers other than first/last are
   cleared after asking.
3. **Detect.** Run the detection pipeline (reused from Fit if already run in
   this invocation). Take every downbeat strictly between first and last.
4. **Assign.** The k-th interior downbeat maps to project bar
   `first bar + k`. Because first and last are pinned and the count is
   known, there is no nearest-bar ambiguity. If the number of interior
   downbeats is not `N - 1`, the pipeline result is inconsistent: report it
   and fall back to nearest-bar assignment within half a bar, skipping any
   bar with no candidate.
5. **Place.** For each interior downbeat, add a stretch marker at its source
   position and set its project position to the bar line. Markers are added
   in source order so REAPER's uniform stretch between neighbours works out.
6. **Report.** Console line: `SRT: "Drums" — conformed 63 bars, largest
   correction 38 ms at bar 41`.

---

## 6. Separation pipeline (srt_core)

Model: BS-RoFormer, 4 stems, host-STFT ONNX export. Input to the model is a
precomputed STFT (real and imaginary parts) of an 8-second stereo chunk at
44.1 kHz. Output is the masked spectrogram per stem. The host performs the
inverse STFT.

1. Split the input into 8-second chunks with 50% overlap.
2. For each chunk: STFT (parameters from the model's manifest), run the
   model, inverse STFT per stem.
3. Overlap-add the stems with a windowed crossfade.
4. Progress reported per chunk.

The STFT and inverse STFT are implemented in srt_core with PocketFFT and
must match PyTorch's `torch.stft` behaviour for the model's window, hop and
padding. This is verified by the correctness test (section 9).

Execution provider order: DirectML (Windows) or CoreML (macOS), then CPU.
The provider used is reported in the console line.

Session creation is slow (seconds). Sessions are cached per model for the
life of the REAPER process.

---

## 7. Detection pipeline (srt_core)

Input: stereo or mono audio at any rate. Output: beat times, downbeat times,
tempo estimate, confidence.

1. **Downmix and resample** to mono 22.05 kHz.
2. **Log-mel spectrogram**: 1024-point FFT, 441-sample hop (20 ms frames),
   128 mel bands, Slaney scale, matching the Beat This! front end exactly.
3. **Run Beat This!** in ONNX Runtime. The model accepts a whole track; for
   a five-minute drum stem that is about 15,000 frames. Long inputs are
   processed in overlapping windows if memory requires.
4. **Decode.** Two options, selected by a setting, default to (b):
   - (a) Peak picking on the model's own output (the paper's default, no
     tempo model).
   - (b) Dynamic Bayesian network decoding (madmom-compatible, reimplemented
     in C++) constrained to the tempo range `project_tempo × [0.85, 1.15]`.
     This uses the project tempo as a prior and removes half/double-tempo
     errors.
5. **Onset refinement.** For each downbeat, compute a spectral-flux onset
   curve at 2 ms resolution over a ±60 ms window and move the downbeat to
   the strongest onset. On a drum stem this gives sample-accurate placement.
6. **Confidence.**
   - high: tempo within 5% of project tempo, downbeat spacing regular
     (standard deviation under 3% of a bar), model downbeat scores strong.
   - medium: one of those fails.
   - low: two or more fail, or tempo outside the 15% constraint.

---

## 8. Models: storage, download, verification

Models live in `<REAPER resource path>/SRT/models/<model-id>/`. Each model
directory holds the ONNX file plus a `manifest.json` describing the front end
(sample rate, FFT size, hop, window, mel settings), input and output names,
chunk size, stem names, version and SHA-256 checksum.

On first use of an action whose model is missing, SRT shows a dialog: model
name, size, source URL, and Download / Cancel. Download runs as a background
job with progress, verifies the checksum, and then the original action runs.

Model files are attached to the GitHub release that matches the extension
version. The extension knows the exact filenames and checksums it was built
with, so a version mismatch is impossible.

Planned models for the first release:

| ID | Purpose | Size | Source |
|---|---|---|---|
| `bsroformer-4s-v1` | Stem separation | ~257 MB (fp16) | ZFTurbo checkpoint, MIT |
| `beatthis-final0-v1` | Beat and downbeat tracking | ~82 MB (fp32) | CPJKU checkpoint, MIT |

---

## 9. Correctness and testing

"Correct" means: given the same input, our C++ output matches the original
Python implementation. That is checked mechanically, not by ear.

### Model correctness tests (run at release time)

For each model, a Python script in `tools/` runs the original PyTorch model
on a fixed set of test clips and stores the outputs. The C++ test (srt_cli)
runs the ONNX model with our front end on the same clips and compares:

- Separation: per-stem waveform difference under -60 dB relative to the
  reference, after aligning any constant delay.
- Beat tracking: model output frames match within 1e-3; decoded beat times
  match within one frame (20 ms).

### Accuracy tests (run in CI on every change)

A test corpus lives outside the repository (large files) and is fetched by
CI. Shaun supplies:

- 3 to 5 real drum stems with known tempo and bar count, plus the starting
  offset of bar 1.
- 1 or 2 full mixes for separation smoke tests.

Assertions:

- Fit: detected bar count equals the known count on every stem; first
  downbeat within 10 ms of the known position.
- Conform: every interior downbeat within 15 ms of a hand-verified reference
  marker list.

### Unit tests

Bar-fitting maths, STFT round trips, WAV read/write, overlap-add, resampler
wrapper. Catch2 or doctest, run on every push.

### Manual test in REAPER

Before each release, a checklist in `docs/TESTING.md` run by Shaun or Dean on
a real project.

---

## 10. Build, dependencies, licensing

- **Build:** CMake 3.24+. Visual Studio 2022 on Windows, Xcode/clang on
  macOS. `cmake --preset windows-release` and `cmake --preset macos-release`.
- **REAPER SDK:** `reaper_plugin.h` and `reaper_plugin_functions.h` from
  the official reaper-sdk repository, vendored as a git submodule. WDL and
  SWELL from the same source for the progress window.
- **ONNX Runtime:** prebuilt binaries fetched by CMake at configure time.
  Windows: the DirectML package. macOS: the standard package with CoreML.
  Shipped next to the extension as `onnxruntime.dll` / `libonnxruntime.dylib`
  and `DirectML.dll`. ReaPack installs them alongside.
- **PocketFFT** (BSD 3-Clause) for FFTs.
- **nlohmann/json** (MIT) for manifests.
- **libcurl** via the platform's system library (WinHTTP on Windows,
  NSURLSession or libcurl on macOS) for model downloads.
- **Catch2** (Boost licence) for tests.
- **License:** BSD 3-Clause for SRT. Every dependency and model is MIT, BSD
  or public domain. No GPL or AGPL anywhere.

---

## 11. Release and distribution

1. Bump `version.txt`, tag `vX.Y.Z`, push the tag.
2. GitHub Actions builds Windows x64 and macOS arm64, runs unit tests and
   model correctness tests, packages the extension plus runtime libraries,
   and attaches the binaries and model files to a GitHub release.
3. The workflow regenerates `index.xml` (ReaPack index) in the repository
   root, listing the new version for both platforms, and commits it.
4. Users import `https://raw.githubusercontent.com/shaunobi/reaper-tools/main/index.xml`
   into ReaPack once and get updates like any other package.

ReaPack installs into `UserPlugins/`. The runtime libraries are listed as
additional files of the same package so they land next to the extension.

---

## 12. Milestones

| # | Milestone | Deliverable |
|---|---|---|
| 0 | Foundation | Repo layout, CMake, CI building an empty extension on both platforms that registers one "SRT: About" action. Background job runner and progress window. ONNX Runtime linked and a "hello" model running on DirectML. |
| 1 | Detection in srt_cli | Beat This! running in C++, front end verified against Python, onset refinement, DBN decoding with tempo prior. `srt_cli beats file.wav` prints downbeats and tempo. Accuracy tests pass on Shaun's stems. |
| 2 | Fit drums to grid | Action wired to REAPER. Manual test on a real project. |
| 3 | Conform drum bars | Action wired. Manual test. First tagged release `v0.1.0` for internal use. |
| 4 | Separation in srt_cli | BS-RoFormer export, STFT front end verified, chunking and overlap-add, `srt_cli separate file.wav`. Correctness test passes. |
| 5 | Separate stems | Action wired, model download flow, folder and track creation. Release `v0.2.0`, public ReaPack index. |
| 6 | macOS | Apple Silicon build with CoreML, same tests. Release `v0.3.0`. |

---

## 13. Open questions and stated assumptions

These were not settled in conversation. The spec proceeds under the stated
assumption; correct any that are wrong.

1. **Bar numbering in projects without lead-in bars.** Assumed: "bar 1"
   means REAPER's displayed measure 1 after the project measure offset. If
   the project shows no measures before 1, first downbeat goes to measure 2.
2. **Lead-in audio.** Assumed: audio before the first downbeat is kept and
   simply sits before bar 1. It is not trimmed.
3. **Audio after the final downbeat.** Assumed: kept, stretched at the same
   ratio as the last bar. Not trimmed.
4. **Stem track naming.** Assumed: folder named `<take name> stems`, child
   tracks named `drums`, `bass`, `vocals`, `other`. Easy to change.
5. **Source rate and stem rate.** Assumed: stems are written at the source
   file's rate, which may differ from the project rate. If Shaun prefers
   project rate, that is a one-line change.
6. **Console versus popup for confidence warnings.** Assumed: console only,
   never a popup, so batch runs are not interrupted.
7. **Downloaded model location.** Assumed: REAPER resource path, so it is
   per user and survives REAPER updates.
8. **Copyright holder for the BSD licence.** Assumed: "Shaunobi's Reaper
   Tools contributors". Replace with a name if preferred.

---

## 14. Future work (not scheduled)

- Tempo changes inside the item (per-bar target from `TimeMap_GetMeasureInfo`
  already makes this mostly free in Conform; Fit needs a bar-by-bar target).
- Beat-level conforming (quarter notes) using the beat list the tracker
  already produces.
- Multi-microphone drum groups: detect on one item, apply markers to all
  items in the group.
- Sub-separation of drums into kick, snare, toms, hats, cymbals.
- Model ensembles and a quality/speed setting.
- CUDA execution provider as an optional download for Nvidia users.
- Windows on ARM, macOS Intel.
