#pragma once
// Pure arithmetic behind "Fit drums to grid" and "Conform drum bars". No
// REAPER types here so every branch is unit-testable. Times are in seconds.
//
// Terms: "source time" is a position inside the take's media file. "Item
// time" is seconds from the item's start on the timeline, before stretch
// markers are applied. REAPER stretch markers pair an item time with a
// source time and stretch uniformly between neighbours.

#include <vector>

namespace srt {

struct StretchMarkerPlan {
    double itemPos; // seconds from the item start
    double srcPos; // seconds in the source media
};

struct GridFitInput {
    double srcStart = 0.0; // source time at the item start (take start offset)
    double srcVisibleLength = 0.0; // seconds of source covered by the item
    double playrate = 1.0; // take playrate; source seconds per item second
    double firstDownbeatSrc = 0.0; // bar 1 downbeat, source time
    double lastDownbeatSrc = 0.0; // final downbeat, source time
    int barCount = 0; // bars between the two downbeats
    double firstBarTime = 0.0; // project time of the target bar line for bar 1
    double lastBarTime = 0.0; // project time of the target bar line barCount bars later
};

struct GridFitPlan {
    double itemPosition = 0.0; // new item start on the timeline
    double itemLength = 0.0; // new item length so the same source stays visible
    StretchMarkerPlan first; // bar 1 marker
    StretchMarkerPlan last; // final downbeat marker
    double stretchRatio = 1.0; // item seconds per original item second between the markers
};

// Throws std::invalid_argument for impossible inputs (non-positive playrate,
// downbeats out of order, bar count < 1, target bars out of order).
GridFitPlan planGridFit(const GridFitInput& in);

// Bar count between two downbeats at a known tempo, rounded to the nearest
// whole bar. beatsPerBar is normally 4.
int barCountFromTempo(double firstDownbeatSrc, double lastDownbeatSrc, double bpm, int beatsPerBar = 4);

// Markers for the interior downbeats once the ends are fixed. interiorSrc
// holds the downbeats strictly between first and last (source time), and
// barTimes the project times of the matching bar lines, in the same order.
// itemPosition and playrate describe the item after planGridFit.
std::vector<StretchMarkerPlan> planConform(double itemPosition, const std::vector<double>& interiorSrc,
                                           const std::vector<double>& barTimes);

// Nearest-bar fallback used when the counts disagree: each downbeat is
// assigned to the closest bar line within half a bar, later downbeats never
// map to an earlier bar, and unmatched bars are skipped. Returns pairs
// (downbeat index, bar index).
std::vector<std::pair<int, int>> assignNearestBars(const std::vector<double>& downbeatTimes,
                                                   const std::vector<double>& barTimes);

} // namespace srt
