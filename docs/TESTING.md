# Manual test checklist (run inside REAPER)

These checks need a real REAPER. Do them on a copy of a project. Every
action prints to the ReaScript console (View > ReaScript console), so keep
it open.

## Install

1. Download the `reaper_srt-windows-x64` artifact from the latest CI run
   (or build with `docs/BUILDING.md`) and copy the contents of its
   `UserPlugins` folder into REAPER's `UserPlugins` folder.
2. Download the beat model and save it as
   `<resource path>/SRT/models/beatthis-final0-v1/beat-this-final0.onnx`
   (the actions print the exact path and URL if it is missing).
3. Restart REAPER. The console should show `SRT: loaded version ...`.

## SRT: About

- Run it from the action list. Console shows version, REAPER version, ONNX
  Runtime version and the provider list. On the Windows studio machine the
  list must include DirectML.
- A small "SRT jobs" window appears with a progress bar for about a second
  and then hides itself. The console ends with `smoke model OK` and names
  the provider used.
- Run it twice quickly: the window shows two rows.

## SRT: Fit drums to grid

Setup: a project at a fixed tempo (say 120) with bars -2 and -1 before bar
1, and a separated drum stem from a song near that tempo (say 121) placed
anywhere on a track, no stretch markers.

1. Select the item, run the action. The jobs window shows progress; the
   console prints the detected tempo, beat and downbeat counts, confidence,
   and the fitted bar count with the bar numbers used.
2. The item has moved so its first downbeat sits exactly on bar 1 (zoom in:
   the transient should start at the bar line). The final downbeat sits on
   bar `1 + N`. Two stretch markers exist, one on each.
3. Play from bar 1 against the metronome. The first and last bar should be
   tight; the middle may drift, that is expected before the next action.
4. Undo once: the item returns to where it was, markers gone.
5. Drag the last stretch marker one bar later by hand, run the action again:
   it re-fits to the adjusted end and reports one more bar.
6. Try an item that already has three or more markers: it asks before
   removing them; answer No and nothing changes.
7. Try a MIDI item: it is skipped with a console message.
8. Try with a project that starts at bar 1: the first downbeat goes to bar 2.

## SRT: Conform drum bars

1. On the fitted item, run the action. A stretch marker appears on every
   downbeat, each snapped to its bar line. Console reports the number of
   interior bars and the largest correction in milliseconds.
2. Play against the metronome: every bar should now be tight.
3. Undo: back to the fitted state with two markers.
4. Run it on an item that was never fitted: it fits and conforms in one go.

## Things to write down for Shaun and Dean

- The detected tempo and bar count for each real stem, and whether they were
  right. Wrong bar counts are the most important thing to report.
- How far the refined downbeats are from where you would have put them by
  ear (a few milliseconds is fine, tens of milliseconds is a bug).
- How long a five-minute stem takes on the studio machine, and which
  provider the console reported.
