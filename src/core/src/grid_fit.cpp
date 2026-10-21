#include "srt/grid_fit.h"

#include <cmath>
#include <stdexcept>

namespace srt {

GridFitPlan planGridFit(const GridFitInput& in) {
    if (in.playrate <= 0.0) throw std::invalid_argument("planGridFit: playrate must be positive");
    if (in.lastDownbeatSrc <= in.firstDownbeatSrc) throw std::invalid_argument("planGridFit: downbeats out of order");
    if (in.barCount < 1) throw std::invalid_argument("planGridFit: need at least one bar");
    if (in.lastBarTime <= in.firstBarTime) throw std::invalid_argument("planGridFit: target bars out of order");
    if (in.srcVisibleLength <= 0.0) throw std::invalid_argument("planGridFit: item has no length");

    GridFitPlan plan;
    // Item time of the first downbeat before any stretching.
    const double firstItemTime = (in.firstDownbeatSrc - in.srcStart) / in.playrate;
    const double lastItemTime = (in.lastDownbeatSrc - in.srcStart) / in.playrate;
    plan.itemPosition = in.firstBarTime - firstItemTime;

    plan.first = {firstItemTime, in.firstDownbeatSrc};
    plan.last = {in.lastBarTime - plan.itemPosition, in.lastDownbeatSrc};
    plan.stretchRatio = (plan.last.itemPos - plan.first.itemPos) / (lastItemTime - firstItemTime);

    // Audio after the last marker keeps the last segment's ratio; keep the
    // same source material visible.
    const double srcEnd = in.srcStart + in.srcVisibleLength;
    const double tailItemSeconds = (srcEnd - in.lastDownbeatSrc) / in.playrate * plan.stretchRatio;
    plan.itemLength = plan.last.itemPos + tailItemSeconds;
    return plan;
}

int barCountFromTempo(double firstDownbeatSrc, double lastDownbeatSrc, double bpm, int beatsPerBar) {
    if (bpm <= 0.0 || beatsPerBar < 1) throw std::invalid_argument("barCountFromTempo: invalid tempo");
    const double barSeconds = 60.0 / bpm * beatsPerBar;
    return static_cast<int>(std::lround((lastDownbeatSrc - firstDownbeatSrc) / barSeconds));
}

std::vector<StretchMarkerPlan> planConform(double itemPosition, const std::vector<double>& interiorSrc,
                                           const std::vector<double>& barTimes) {
    if (interiorSrc.size() != barTimes.size()) throw std::invalid_argument("planConform: size mismatch");
    std::vector<StretchMarkerPlan> out;
    out.reserve(interiorSrc.size());
    for (size_t i = 0; i < interiorSrc.size(); ++i) out.push_back({barTimes[i] - itemPosition, interiorSrc[i]});
    return out;
}

std::vector<std::pair<int, int>> assignNearestBars(const std::vector<double>& downbeatTimes,
                                                   const std::vector<double>& barTimes) {
    std::vector<std::pair<int, int>> out;
    if (barTimes.size() < 2) return out;
    int nextBar = 0;
    for (size_t d = 0; d < downbeatTimes.size(); ++d) {
        const double t = downbeatTimes[d];
        int best = -1;
        double bestDist = 1e300;
        for (int b = nextBar; b < static_cast<int>(barTimes.size()); ++b) {
            const double dist = std::fabs(barTimes[static_cast<size_t>(b)] - t);
            if (dist < bestDist) {
                bestDist = dist;
                best = b;
            }
            if (barTimes[static_cast<size_t>(b)] > t) break;
        }
        if (best < 0) continue;
        const size_t bi = static_cast<size_t>(best);
        const double barLen = bi + 1 < barTimes.size() ? barTimes[bi + 1] - barTimes[bi] : barTimes[bi] - barTimes[bi - 1];
        if (bestDist <= 0.5 * barLen) {
            out.push_back({static_cast<int>(d), best});
            nextBar = best + 1;
        }
    }
    return out;
}

} // namespace srt
