# SRT — Shaunobi's Reaper Tools

A REAPER extension, written in C++, that adds studio-workflow actions in the
style of SWS. Distributed through ReaPack.

Planned actions for the first releases:

- **SRT: Separate stems** — split a selected item into drums, bass, vocals
  and other using an open-weight model running locally (ONNX Runtime,
  DirectML on Windows, CoreML on macOS).
- **SRT: Fit drums to grid** — detect the first and last downbeat of a drum
  item, work out the bar count, and move and stretch it so both sit on the
  project's bar lines.
- **SRT: Conform drum bars** — put a stretch marker on every downbeat in
  between and snap each to its bar line.

The full specification, including every design decision and the open
questions, is in [docs/SPEC.md](docs/SPEC.md).

License: BSD 3-Clause. See [LICENSE](LICENSE).
