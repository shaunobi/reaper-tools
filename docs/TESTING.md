# Manual test checklist (run inside REAPER)

These checks need a real REAPER. Do them on a copy of a project. Every
action reports to the SRT log window: run "SRT: Show/hide log window" from
the action list and leave it open. It keeps everything from this session
and the end of earlier ones (the file is `<resource path>/SRT/srt.log`);
closing it only hides it. Nothing is ever written to the ReaScript
console. The log window opens by itself only when an error is logged.

## Install

1. Download the `reaper_srt-windows-x64` artifact from the latest CI run
   (or build with `docs/BUILDING.md`) and copy the contents of its
   `UserPlugins` folder into REAPER's `UserPlugins` folder.
2. Restart REAPER. Models download on first use: the action asks first,
   shows progress in the jobs window, and verifies the file. If the download
   cannot run, the console prints the URL and the exact path to save to.
3. Restart REAPER. Nothing should pop up. Open the log window: it shows
   `SRT: loaded version ...` with a timestamp, under a line marking where
   the previous session's log ends.

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
7a. A recording with a faint count-in or click bleed before the first hit:
    the console says how many lead-in beats were ignored, and the first
    real hit is the first downbeat. Nothing before it counts as a bar.
8. Try with a project that starts at bar 1: the first downbeat goes to bar 2.
8a. Same project, a recording with more than a bar of lead-in before the
    first hit (a count-in): the console says the lead-in does not fit before
    bar 2 and names the bar used instead; the item starts at or after 0 and
    the markers sit exactly on bar lines.
9. Tempo guard: set the project tempo well away from the recording (a
   136 BPM take in a 120 BPM project). The action asks before fitting,
   naming both tempos and the stretch; No leaves the item untouched, Yes
   fits with the large stretch as before.

## SRT: Detect tempo

Setup: a project at some tempo (say 120, no tempo map) with a drum recording
at a clearly different tempo, plus one or two other items on other tracks.

1. Select the recording, run the action. The console reports the detected
   tempo, counts and confidence, how far the project tempo is from it, and
   the stretch Fit would need. Nothing has changed yet.
2. The question that follows names the detected tempo and says how many
   other items follow the tempo (and will move or stretch) and how many are
   on the Time base (and will stay put). Check those counts against the
   items' timebase settings (item properties, or the project default). If
   the recording itself is on a Beats timebase (REAPER's default), the
   question also says it will be set to the Time base.
3. Answer No: the console says the tempo was left alone; nothing changed.
4. Run again, answer Yes: the project tempo changes to the detected value,
   the other items react as the question said, and the recording slides so
   its first hit sits on the bar line the console names (bar 1 with lead-in
   bars, else bar 2, or later if the lead-in does not fit). It keeps its
   length and playrate (item properties: timebase is now Time, no stretch
   markers); it must not shrink or stretch. The small snap-offset triangle
   at the bottom of the item sits on the first hit: drag the item with snap
   on and the hit, not the item edge, snaps to the grid. One undo step
   restores all.
5. Add a tempo marker somewhere, run again: it reports but does not offer
   to change anything, and says the project has a tempo map.

## SRT: Conform drum bars

1. On the fitted item, run the action. A stretch marker appears on every
   downbeat, each snapped to its bar line. Console reports the number of
   interior bars and the largest correction in milliseconds.
2. Play against the metronome: every bar should now be tight.
3. Undo: back to the fitted state with two markers.
4. Run it on an item that was never fitted: it fits and conforms in one go.

## SRT: Separate stems

Setup: a full mix (a few minutes long) on a track. Keep the ReaScript
console open.

1. Select the item, run the action. The first time, a dialog asks to
   download the 269 MB model; accept. The jobs window shows the download
   with a megabyte counter, then "verifying checksum", then the separation
   starts by itself.
2. While it runs, keep editing elsewhere in the project. REAPER must stay
   responsive. Cancel from the jobs window once to check that nothing is
   written and nothing changes; then run it again.
3. When it finishes, a folder track named "<take> stems" appears directly
   below the source track with four child tracks: drums, bass, other,
   vocals. Each holds one item at exactly the source item's position and
   length. The source item and track are untouched.
4. The console line reports the time taken and the provider. On the studio
   machine it must say DirectML; if it says CPU, DirectML failed and the
   run will have taken minutes rather than seconds.
5. Solo the drums track: it should be clean drums. Play all four stems
   together against the muted source: they should sum to the original.
6. Undo once: the folder and its tracks disappear (the WAV files stay in the
   project media folder).
7. Run it on an item that is a trimmed section of a longer file: the stems
   cover only the visible section.
8. Run it with two items selected: two folders, one per item, each below
   its own source track.

## Things to write down for Shaun and Dean

- The detected tempo and bar count for each real stem, and whether they were
  right. Wrong bar counts are the most important thing to report.
- How far the refined downbeats are from where you would have put them by
  ear (a few milliseconds is fine, tens of milliseconds is a bug).
- How long a five-minute stem takes on the studio machine, and which
  provider the console reported.
- For separation: how the drums stem compares with what UVR gives you on
  the same song. That is the quality bar the model choice is measured
  against.
