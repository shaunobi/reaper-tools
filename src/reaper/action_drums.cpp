// "SRT: Fit drums to grid" and "SRT: Conform drum bars". See docs/SPEC.md
// section 5. Both run the detection pipeline on a background thread and
// apply the result on the main thread inside one undo block.

#include "actions.h"
#include "host.h"
#include "item_audio.h"
#include "models.h"
#include "timeline.h"

#include "srt/beat_detect.h"
#include "srt/grid_fit.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace srt::reaper {

namespace {

constexpr const char* kExtFirst = "P_EXT:SRT_FIRST"; // source time of the bar 1 marker
constexpr const char* kExtLast = "P_EXT:SRT_LAST"; // source time of the final downbeat marker
constexpr const char* kExtBars = "P_EXT:SRT_BARS"; // bar count between them

std::string fmt(double v, int decimals = 3) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

std::string getExt(MediaItem* item, const char* key) {
    char buf[512] = {0};
    if (!GetSetMediaItemInfo_String(item, key, buf, false)) return {};
    return buf;
}

void setExt(MediaItem* item, const char* key, const std::string& value) {
    std::string v = value;
    GetSetMediaItemInfo_String(item, key, v.data(), true);
}

bool askYesNo(const std::string& question, const char* title) {
    return ShowMessageBox(question.c_str(), title, 4) == 6;
}

std::string takeLabel(MediaItem_Take* take) {
    const char* name = GetTakeName(take);
    return name && *name ? name : "(unnamed take)";
}

struct ItemContext {
    ReaProject* proj = nullptr;
    MediaItem* item = nullptr;
    MediaItem_Take* take = nullptr;
    std::string label;
    double srcStart = 0.0; // take start offset
    double srcVisibleLength = 0.0;
    double playrate = 1.0;
    double tempoHint = 0.0;
    // User-supplied ends (source time), or negative when they must be detected.
    double userFirstSrc = -1.0;
    double userLastSrc = -1.0;
    int userBars = 0;
};

// Gathers everything the job and the apply step need, handling the stretch
// marker questions. Returns false if the item is skipped.
bool prepareItem(MediaItem* item, ItemContext& ctx, bool conform) {
    ctx.item = item;
    ctx.proj = GetItemProjectContext(item);
    ctx.take = GetActiveTake(item);
    if (!ctx.take) {
        console("skipped an item with no active take");
        return false;
    }
    ctx.label = takeLabel(ctx.take);
    SourceInfo info = describeSource(ctx.take);
    if (!info.isAudio) {
        console("skipped \"" + ctx.label + "\": not an audio take (" + info.type + ")");
        return false;
    }

    // Existing SRT ends?
    const std::string extFirst = getExt(item, kExtFirst), extLast = getExt(item, kExtLast);
    const bool hasState = !extFirst.empty() && !extLast.empty();
    const int markers = GetTakeNumStretchMarkers(ctx.take);

    if (hasState) {
        // The current outermost markers win, so hand adjustments are honoured;
        // fall back to the stored values if the markers are gone.
        ctx.userFirstSrc = std::atof(extFirst.c_str());
        ctx.userLastSrc = std::atof(extLast.c_str());
        if (markers >= 2) {
            double pos = 0.0, src = 0.0;
            if (GetTakeStretchMarker(ctx.take, 0, &pos, &src) >= 0) ctx.userFirstSrc = src;
            if (GetTakeStretchMarker(ctx.take, markers - 1, &pos, &src) >= 0) ctx.userLastSrc = src;
        }
        ctx.userBars = std::atoi(getExt(item, kExtBars).c_str());
        if (!conform) {
            // Fit again: keep the ends the user (or we) already have, clear the rest.
            if (markers > 2 &&
                !askYesNo("\"" + ctx.label + "\" has " + std::to_string(markers) +
                              " stretch markers. Remove them (keeping the first/last bar positions) and re-fit?",
                          "SRT: Fit drums to grid"))
                return false;
        }
    } else if (markers == 2) {
        if (askYesNo("\"" + ctx.label + "\" has two stretch markers. Use them as the first and last bar?",
                     conform ? "SRT: Conform drum bars" : "SRT: Fit drums to grid")) {
            double pos = 0.0, src = 0.0;
            GetTakeStretchMarker(ctx.take, 0, &pos, &src);
            ctx.userFirstSrc = src;
            GetTakeStretchMarker(ctx.take, 1, &pos, &src);
            ctx.userLastSrc = src;
        } else if (!askYesNo("Remove the two stretch markers and detect the bars instead?", "SRT")) {
            return false;
        }
    } else if (markers > 0) {
        if (!askYesNo("\"" + ctx.label + "\" has " + std::to_string(markers) + " stretch markers. Remove them and continue?",
                      "SRT"))
            return false;
    }

    // Clear all markers now so the item's source range is plain again.
    if (markers > 0) {
        Undo_BeginBlock2(ctx.proj);
        DeleteTakeStretchMarkers(ctx.take, 0, &markers);
        Undo_EndBlock2(ctx.proj, "SRT: clear stretch markers", -1);
    }

    ctx.srcStart = GetMediaItemTakeInfo_Value(ctx.take, "D_STARTOFFS");
    ctx.playrate = GetMediaItemTakeInfo_Value(ctx.take, "D_PLAYRATE");
    if (ctx.playrate <= 0.0) ctx.playrate = 1.0;
    ctx.srcVisibleLength = GetMediaItemInfo_Value(item, "D_LENGTH") * ctx.playrate;
    if (ctx.srcStart + ctx.srcVisibleLength > info.lengthSeconds) ctx.srcVisibleLength = info.lengthSeconds - ctx.srcStart;
    if (ctx.srcVisibleLength <= 0.5) {
        console("skipped \"" + ctx.label + "\": item is too short");
        return false;
    }
    ctx.tempoHint = tempoAt(GetMediaItemInfo_Value(item, "D_POSITION"));
    return true;
}

struct DrumJob : srt::Job {
    ItemContext ctx;
    bool conform = false;
    std::filesystem::path modelPath;
    srt::AudioBuffer audio;
    srt::BeatDetectResult result;

    std::string name() const override {
        return std::string(conform ? "Conform drum bars: " : "Fit drums to grid: ") + ctx.label;
    }
    void run(srt::Progress& p) override {
        p.set(0.01, "loading model");
        srt::BeatTrackerConfig cfg;
        cfg.modelPath = modelPath;
        srt::BeatTracker tracker(cfg);
        srt::BeatDetectOptions opt;
        opt.tempoHintBpm = ctx.tempoHint;
        result = srt::detectBeats(tracker, audio, opt, &p);
    }
};

// Everything below runs on the main thread after the job finishes.

struct Ends {
    double firstSrc = 0.0;
    double lastSrc = 0.0;
    int bars = 0;
};

bool chooseEnds(const DrumJob& job, Ends& ends, std::vector<double>& downbeatsSrc) {
    const ItemContext& ctx = job.ctx;
    downbeatsSrc.clear();
    for (double t : job.result.events.downbeats) downbeatsSrc.push_back(ctx.srcStart + t);

    if (ctx.userFirstSrc >= 0.0 && ctx.userLastSrc > ctx.userFirstSrc) {
        ends.firstSrc = ctx.userFirstSrc;
        ends.lastSrc = ctx.userLastSrc;
        // Prefer counting the detected downbeats between the ends; fall back
        // to the tempo when the detector found too few.
        const double bar = job.result.tempoBpm > 0.0 ? 4.0 * 60.0 / job.result.tempoBpm : 0.0;
        int counted = 0;
        for (double s : downbeatsSrc)
            if (s >= ends.firstSrc - 0.5 * bar && s <= ends.lastSrc + 0.5 * bar) ++counted;
        const int fromTempo = bar > 0.0 ? srt::barCountFromTempo(ends.firstSrc, ends.lastSrc, job.result.tempoBpm) : 0;
        ends.bars = counted >= 2 ? counted - 1 : fromTempo;
        if (counted >= 2 && fromTempo != ends.bars)
            console("  bar count: " + std::to_string(ends.bars) + " downbeats counted, " + std::to_string(fromTempo) +
                    " expected from the tempo; using the downbeat count");
        return ends.bars >= 1;
    }
    if (downbeatsSrc.size() < 2) return false;
    ends.firstSrc = downbeatsSrc.front();
    ends.lastSrc = downbeatsSrc.back();
    ends.bars = static_cast<int>(downbeatsSrc.size()) - 1;
    return true;
}

void applyFit(const DrumJob& job, const Ends& ends, srt::GridFitPlan& plan, int& firstMeasure) {
    const ItemContext& ctx = job.ctx;
    firstMeasure = targetMeasureForBarOne(ctx.proj);
    srt::GridFitInput in;
    in.srcStart = ctx.srcStart;
    in.srcVisibleLength = ctx.srcVisibleLength;
    in.playrate = ctx.playrate;
    in.firstDownbeatSrc = ends.firstSrc;
    in.lastDownbeatSrc = ends.lastSrc;
    in.barCount = ends.bars;
    in.firstBarTime = measureStartTime(ctx.proj, firstMeasure);
    in.lastBarTime = measureStartTime(ctx.proj, firstMeasure + ends.bars);
    plan = srt::planGridFit(in);

    SetMediaItemPosition(ctx.item, plan.itemPosition, false);
    SetMediaItemLength(ctx.item, plan.itemLength, false);
    SetTakeStretchMarker(ctx.take, -1, plan.first.itemPos, &plan.first.srcPos);
    SetTakeStretchMarker(ctx.take, -1, plan.last.itemPos, &plan.last.srcPos);
    setExt(ctx.item, kExtFirst, fmt(ends.firstSrc, 6));
    setExt(ctx.item, kExtLast, fmt(ends.lastSrc, 6));
    setExt(ctx.item, kExtBars, std::to_string(ends.bars));
}

void applyConform(const DrumJob& job, const Ends& ends, const srt::GridFitPlan& plan, int firstMeasure,
                  const std::vector<double>& downbeatsSrc) {
    const ItemContext& ctx = job.ctx;
    std::vector<double> interior;
    for (double s : downbeatsSrc)
        if (s > ends.firstSrc + 0.05 && s < ends.lastSrc - 0.05) interior.push_back(s);

    std::vector<double> barTimes;
    for (int b = 1; b < ends.bars; ++b) barTimes.push_back(measureStartTime(ctx.proj, firstMeasure + b));

    std::vector<srt::StretchMarkerPlan> markers;
    if (interior.size() == barTimes.size()) {
        markers = srt::planConform(plan.itemPosition, interior, barTimes);
    } else {
        // Counts disagree: place each downbeat on its nearest bar within half a bar.
        console("  found " + std::to_string(interior.size()) + " interior downbeats for " +
                std::to_string(barTimes.size()) + " bars; using nearest-bar assignment");
        std::vector<double> projTimes;
        for (double s : interior)
            projTimes.push_back(plan.itemPosition + plan.first.itemPos +
                                (s - ends.firstSrc) / ctx.playrate * plan.stretchRatio);
        for (auto [d, b] : srt::assignNearestBars(projTimes, barTimes))
            markers.push_back({barTimes[static_cast<size_t>(b)] - plan.itemPosition, interior[static_cast<size_t>(d)]});
    }

    double largest = 0.0;
    int largestBar = 0;
    for (size_t i = 0; i < markers.size(); ++i) {
        const double naturalPos = plan.first.itemPos + (markers[i].srcPos - ends.firstSrc) / ctx.playrate * plan.stretchRatio;
        const double correction = std::fabs(markers[i].itemPos - naturalPos);
        if (correction > largest) {
            largest = correction;
            largestBar = static_cast<int>(i) + 2;
        }
        SetTakeStretchMarker(ctx.take, -1, markers[i].itemPos, &markers[i].srcPos);
    }
    console("conformed \"" + ctx.label + "\": " + std::to_string(markers.size()) + " interior bars, largest correction " +
            fmt(largest * 1000.0, 0) + " ms at bar " + std::to_string(largestBar));
}

void onJobDone(const srt::JobStatus& st, srt::Job& base) {
    auto& job = static_cast<DrumJob&>(base);
    const ItemContext& ctx = job.ctx;
    if (st.state == srt::JobState::Cancelled) {
        console("cancelled \"" + ctx.label + "\"");
        return;
    }
    if (st.state != srt::JobState::Succeeded) {
        console("failed on \"" + ctx.label + "\": " + st.error);
        return;
    }
    if (!ValidatePtr2(ctx.proj, ctx.item, "MediaItem*") || !ValidatePtr2(ctx.proj, ctx.take, "MediaItem_Take*")) {
        console("\"" + ctx.label + "\" is gone; nothing applied");
        return;
    }

    const srt::BeatDetectResult& r = job.result;
    std::ostringstream line;
    line << "\"" << ctx.label << "\": " << fmt(r.tempoBpm, 1) << " BPM detected (project " << fmt(ctx.tempoHint, 1)
         << "), " << r.events.beats.size() << " beats, " << r.events.downbeats.size() << " downbeats, confidence "
         << srt::toString(r.confidence) << " (" << srt::toString(r.provider) << ")";
    console(line.str());
    for (const auto& n : r.notes) console("  " + n);

    Ends ends;
    std::vector<double> downbeatsSrc;
    if (!chooseEnds(job, ends, downbeatsSrc)) {
        console("  could not find two downbeats; nothing applied");
        return;
    }

    Undo_BeginBlock2(ctx.proj);
    srt::GridFitPlan plan;
    int firstMeasure = 0;
    try {
        applyFit(job, ends, plan, firstMeasure);
        const int shownFirst = displayedBarForMeasure(ctx.proj, firstMeasure);
        console("  fitted " + std::to_string(ends.bars) + " bars: first downbeat at source " + fmt(ends.firstSrc) +
                " s -> bar " + std::to_string(shownFirst) + ", last downbeat at source " + fmt(ends.lastSrc) +
                " s -> bar " + std::to_string(shownFirst + ends.bars) + ", stretch ratio " + fmt(plan.stretchRatio, 5));
        if (job.conform) applyConform(job, ends, plan, firstMeasure, downbeatsSrc);
    } catch (const std::exception& ex) {
        console(std::string("  error: ") + ex.what());
    }
    UpdateItemInProject(ctx.item);
    Undo_EndBlock2(ctx.proj, job.conform ? "SRT: Conform drum bars" : "SRT: Fit drums to grid", -1);
    UpdateArrange();
}

void runDrumActionWithModel(bool conform, const std::filesystem::path& model) {
    const int count = CountSelectedMediaItems(nullptr);
    if (count == 0) {
        console("select at least one drum item first");
        return;
    }
    std::vector<MediaItem*> items;
    for (int i = 0; i < count; ++i) items.push_back(GetSelectedMediaItem(nullptr, i));

    for (MediaItem* item : items) {
        auto job = std::make_shared<DrumJob>();
        job->conform = conform;
        job->modelPath = model;
        if (!prepareItem(item, job->ctx, conform)) continue;
        try {
            job->audio = readSourceAudio(job->ctx.take, job->ctx.srcStart, job->ctx.srcVisibleLength);
        } catch (const std::exception& ex) {
            console("skipped \"" + job->ctx.label + "\": " + ex.what());
            continue;
        }
        console(std::string(conform ? "conforming" : "fitting") + " \"" + job->ctx.label + "\" (" +
                fmt(job->audio.durationSeconds(), 1) + " s of audio, project tempo " + fmt(job->ctx.tempoHint, 1) + " BPM)");
        jobs().submit(job, &onJobDone);
    }
}

void runDrumAction(bool conform) {
    if (CountSelectedMediaItems(nullptr) == 0) {
        console("select at least one drum item first");
        return;
    }
    ensureModel(kBeatThisModel, [conform](const std::filesystem::path& model) { runDrumActionWithModel(conform, model); });
}

} // namespace

void actionFitDrumsToGrid() { runDrumAction(false); }
void actionConformDrumBars() { runDrumAction(true); }

} // namespace srt::reaper
