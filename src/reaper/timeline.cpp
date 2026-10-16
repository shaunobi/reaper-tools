#include "timeline.h"

#include <cstdlib>
#include <cstring>

namespace srt::reaper {

double measureStartTime(ReaProject* proj, int measureIndex) {
    return TimeMap2_beatsToTime(proj, 0.0, &measureIndex);
}

int displayedBarForMeasure(ReaProject* proj, int measureIndex) {
    // Format "measures.beats" for a point just inside the measure and read the
    // leading integer, which is exactly what the ruler shows.
    char buf[128] = {0};
    format_timestr_pos(measureStartTime(proj, measureIndex) + 1e-4, buf, sizeof(buf), 2);
    return std::atoi(buf);
}

int measureIndexForDisplayedBar(ReaProject* proj, int displayedBar) {
    const int first = displayedBarForMeasure(proj, 0);
    int guess = displayedBar - first;
    if (guess < 0) return -1;
    for (int tries = 0; tries < 8; ++tries) {
        const int shown = displayedBarForMeasure(proj, guess);
        if (shown == displayedBar) return guess;
        guess += displayedBar - shown;
        if (guess < 0) return -1;
    }
    return -1;
}

int targetMeasureForBarOne(ReaProject* proj) {
    const int barOne = measureIndexForDisplayedBar(proj, 1);
    if (barOne > 0) return barOne; // there are bars before bar 1
    const int barTwo = measureIndexForDisplayedBar(proj, 2);
    return barTwo >= 0 ? barTwo : 1;
}

double tempoAt(double time) { return TimeMap_GetDividedBpmAtTime(time); }

} // namespace srt::reaper
