# SRT: Shaunobi's Reaper Tools

A free [REAPER](https://www.reaper.fm) extension for studio work with live
recordings: split a mix into stems, find the tempo of a take, and pull a
drum recording onto the project grid bar by bar. Everything runs on your own
machine, using open-weight models through
[ONNX Runtime](https://onnxruntime.ai) (on the graphics card where there is
one). No account, no upload, no subscription.

## Install

1. **Download** the zip for your platform from the
   [latest release](https://github.com/shaunobi/reaper-tools/releases/latest):
   - Windows 10/11, 64-bit: [reaper_srt-windows-x64.zip](https://github.com/shaunobi/reaper-tools/releases/latest/download/reaper_srt-windows-x64.zip)
   - macOS, Apple Silicon: [reaper_srt-macos-arm64.zip](https://github.com/shaunobi/reaper-tools/releases/latest/download/reaper_srt-macos-arm64.zip)
2. **Unzip it into REAPER's `UserPlugins` folder.** To find that folder, in
   REAPER choose *Options > Show REAPER resource path in explorer/finder*;
   `UserPlugins` is inside it. Copy every file from the zip there (the
   extension plus the runtime libraries it needs).
3. **Restart REAPER.** In the action list (shortcut `?`), filter for `SRT:`
   and you will see the actions below. Run *SRT: About* once: it reports the
   version and which engine it will use (DirectML on Windows means your
   graphics card; CPU works too, just slower).

Prefer ReaPack? Import this repository once (*Extensions > ReaPack > Import
repositories*) and updates arrive with everything else:

```
https://raw.githubusercontent.com/shaunobi/reaper-tools/main/index.xml
```

The first time an action needs a model it asks before downloading it (82 MB
for beat detection, 697 MB for stem separation), shows progress, verifies the
file, and keeps it under `<resource path>/SRT/models` for next time.

Requirements: REAPER 7, 64-bit. On Windows the Visual C++ runtime
(2015 to 2022) must be installed; nearly every machine already has it, and it
is a free
[download from Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe)
otherwise. The macOS build is produced and tested in CI but has not yet been
tried on a real Mac; reports welcome.

## What you can do with it

Every action works on the selected media items and keeps REAPER usable while
it runs: a small jobs window shows progress and lets you cancel, and the
result is a single undo step. Details go to the SRT log window
(*SRT: Show/hide log window*).

### Split a song into stems

Select an item holding a full mix and run **SRT: Separate stems**. A folder
track appears under it with four child tracks, drums, bass, other and
vocals, each holding a new item at exactly the source item's position. The
source is untouched. This uses Demucs v4 (fine-tuned), the same model behind
most of the "AI stem splitter" services, running locally.

### Pull a drum take onto the grid

Your drummer played a four-bar phrase at 121 BPM and the project is at 120.
Select the drum item and run **SRT: Fit drums to grid**. It finds the first
and last downbeat, counts the bars between them, moves the item so bar 1
lands on bar 1 of the project (or bar 2 if the project starts at bar 1) and
places two stretch markers so the last downbeat lands exactly N bars later.
The log says what it did:

```
"drums take 3": 121.2 BPM detected (project 120.0), 20 beats, 5 downbeats, confidence high (DirectML)
  fitted 4 bars: first downbeat at source 0.351 s -> bar 1, last downbeat at source 8.265 s -> bar 5, stretch ratio 1.01087
```

If the take is a long way from the project tempo it asks before stretching,
because a large stretch is audible. Drag either end marker by hand and run
the action again: it keeps your ends and refits.

### Tighten every bar in between

Run **SRT: Conform drum bars** on the same item and it adds a stretch marker
on every downbeat, each snapped to its bar line, so the feel inside the bars
stays the drummer's while every bar line is exact. The log reports the
largest correction it made.

### Start a project from a live recording

You have a take and no idea of its tempo. Select it and run
**SRT: Detect tempo**. It reports the tempo, the beat and bar counts, and
how far the project tempo is from it, and changes nothing. In a project
without a tempo map it then offers to set the project tempo to the
recording, telling you first how many other items would move with the
tempo and how many would stay put, and slides the item so its first
downbeat sits on the bar.

## More

- [docs/SPEC.md](docs/SPEC.md): what each action does in detail and every
  design decision behind it.
- [docs/BUILDING.md](docs/BUILDING.md): building from source on Windows,
  macOS or Linux, the test suite, and the fake-REAPER harness.
- [docs/TESTING.md](docs/TESTING.md): the manual checklist run inside REAPER
  before a release.
- Problems or ideas: [open an issue](https://github.com/shaunobi/reaper-tools/issues).

Models: Demucs v4 fine-tuned (Meta AI, MIT) exported by `tools/export_demucs.py`;
Beat This! (CPJKU, MIT), ONNX export by aaatmy. Extension code is
BSD 3-Clause, see [LICENSE](LICENSE).
