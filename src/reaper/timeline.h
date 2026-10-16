#pragma once
// Tempo map helpers. REAPER numbers measures from a project-specific start,
// so "bar 1" as the user sees it is found by asking REAPER how it formats
// each measure's start time.

#include "reaper_api.h"

#include <string>

namespace srt::reaper {

// Measure index whose displayed number is `displayedBar`, or -1 if none in
// the first few thousand measures.
int measureIndexForDisplayedBar(ReaProject* proj, int displayedBar);

// Displayed bar number of a measure index.
int displayedBarForMeasure(ReaProject* proj, int measureIndex);

// Project time of the start of a measure (index, not displayed number).
double measureStartTime(ReaProject* proj, int measureIndex);

// The measure index the first downbeat should land on: displayed bar 1 when
// the project has bars before it, otherwise displayed bar 2.
int targetMeasureForBarOne(ReaProject* proj);

// Tempo in effect at a project time.
double tempoAt(double time);

} // namespace srt::reaper
