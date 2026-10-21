// "SRT: Fit drums to grid", "SRT: Conform drum bars" and "SRT: Detect tempo".
// See docs/SPEC.md section 5. All run the detection pipeline on a background
// thread and apply the result on the main thread inside one undo block.

#include "actions.h"
#include "crash_guard.h"
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
#include <stdexcept>
#include <string>
#include <vector>

namespace srt::reaper {

namespace {

constexpr const char* kExtFirst = "P_EXT:SRT_FIRST"; // source time of the bar 1 marker
constexpr const char* kExtLast = "P_EXT:SRT_LAST"; // source time of the final downbeat marker
constexpr const char* kExtBars = "P_EXT:SRT_BARS"; // bar count between them

// Fit and Conform stop and ask when the recording's tempo is further than
// this from the project tempo: the stretch would be audible, and the user
// may have meant to set the project tempo from the recording instead.
constexpr double kTempoGuardPercent = 5.0;

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

// For "Detect tempo": the item is only read, so no stretch marker questions.
bool prepareItemForDetect(MediaItem* item, ItemContext& ctx) {
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
    bool detectOnly = false; // "Detect tempo": wide search, no project-tempo prior
    std::filesystem::path modelPath;
    srt::AudioBuffer audio;
    srt::BeatDetectResult result;

    std::string name() const override {
        const char* what = detectOnly ? "Detect tempo: " : conform ? "Conform drum bars: " : "Fit drums to grid: ";
        return std::string(what) + ctx.label;
    }
    void run(srt::Progress& p) override {
        p.set(0.01, "loading model");
        std::string fault;
        const bool ok = runGuarded(
            [&] {
                srt::BeatTrackerConfig cfg;
                cfg.modelPath = modelPath;
                srt::BeatTracker tracker(cfg);
                srt::BeatDetectOptions opt;
                opt.tempoHintBpm = detectOnly ? 0.0 : ctx.tempoHint;
                result = srt::detectBeats(tracker, audio, opt, &p);
            },
            fault);
        if (!ok) throw std::runtime_error(fault);
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

    // The lead-in before the first downbeat is kept, so the item must start
    // that much before the target bar line. REAPER cannot place an item
    // before the project start (it would silently stay at 0 and the whole
    // fit would land late), so move the target to the first bar that leaves
    // room.
    const int wanted = firstMeasure;
    while (plan.itemPosition < 0.0 && firstMeasure - wanted < 64) {
        ++firstMeasure;
        in.firstBarTime = measureStartTime(ctx.proj, firstMeasure);
        in.lastBarTime = measureStartTime(ctx.proj, firstMeasure + ends.bars);
        plan = srt::planGridFit(in);
    }
    if (firstMeasure != wanted)
        console("  the " + fmt((ends.firstSrc - ctx.srcStart) / ctx.playrate, 2) +
                " s of audio before the first downbeat does not fit before bar " +
                std::to_string(displayedBarForMeasure(ctx.proj, wanted)) + "; using bar " +
                std::to_string(displayedBarForMeasure(ctx.proj, firstMeasure)) +
                " instead (add lead-in bars to the project to use bar " +
                std::to_string(displayedBarForMeasure(ctx.proj, wanted)) + ")");

    SetMediaItemPosition(ctx.item, plan.itemPosition, false);
    SetMediaItemLength(ctx.item, plan.itemLength, false);
    SetTakeStretchMarker(ctx.take, -1, plan.first.itemPos, &plan.first.srcPos);
    SetTakeStretchMarker(ctx.take, -1, plan.last.itemPos, &plan.last.srcPos);
    // Snap offset on the first downbeat, so a hand nudge with snap on keeps bar 1 on the grid.
    SetMediaItemInfo_Value(ctx.item, "D_SNAPOFFSET", plan.first.itemPos);
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

    if (ctx.tempoHint > 0.0 && r.tempoBpm > 0.0) {
        const double pct = (r.tempoBpm / ctx.tempoHint - 1.0) * 100.0;
        if (std::fabs(pct) > kTempoGuardPercent) {
            const std::string q = "\"" + ctx.label + "\": the recording is at " + fmt(r.tempoBpm, 1) +
                                  " BPM but the project is at " + fmt(ctx.tempoHint, 1) + " BPM here (" + fmt(pct, 1) +
                                  "% apart). Fitting it to the grid would stretch the audio by about " +
                                  fmt(std::fabs(pct), 0) + "%.\n\nContinue anyway?\n\n(No leaves the item untouched. " +
                                  "\"SRT: Detect tempo\" can set the project tempo from the recording instead.)";
            if (!askYesNo(q, job.conform ? "SRT: Conform drum bars" : "SRT: Fit drums to grid")) {
                console("  not applied: the tempo differs from the project by " + fmt(pct, 1) + "%");
                return;
            }
        }
    }

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

// Project default item timebase: 0 = time, 1 = beats (position, length,
// rate), 2 = beats (position only). REAPER's own default is 1.
int projectItemTimebase(ReaProject* proj) {
    int size = 0;
    const int offset = projectconfig_var_getoffs("itemtimelock", &size);
    if (offset < 0 || size != static_cast<int>(sizeof(int))) return 1;
    const void* p = projectconfig_var_addr(proj, offset);
    return p ? *static_cast<const int*>(p) : 1;
}

// Effective timebase of an item (its own, else its track's, else the project's).
int itemTimebase(ReaProject* proj, MediaItem* item) {
    int mode = static_cast<int>(GetMediaItemInfo_Value(item, "C_BEATATTACHMODE"));
    if (mode < 0) {
        MediaTrack* track = GetMediaItem_Track(item);
        if (track) mode = static_cast<int>(GetMediaTrackInfo_Value(track, "C_BEATATTACHMODE"));
    }
    return mode < 0 ? projectItemTimebase(proj) : mode;
}

void onDetectDone(const srt::JobStatus& st, srt::Job& base) {
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
    const srt::BeatDetectResult& r = job.result;
    if (r.tempoBpm <= 0.0 || r.events.beats.size() < 4) {
        console("\"" + ctx.label + "\": no tempo found");
        for (const auto& n : r.notes) console("  " + n);
        return;
    }
    std::ostringstream line;
    line << "\"" << ctx.label << "\": " << fmt(r.tempoBpm, 1) << " BPM detected, " << r.events.beats.size() << " beats, "
         << r.events.downbeats.size() << " downbeats in " << fmt(job.audio.durationSeconds(), 1) << " s, confidence "
         << srt::toString(r.confidence) << " (" << srt::toString(r.provider) << ")";
    console(line.str());
    for (const auto& n : r.notes) console("  " + n);
    console("  if this looks like half or double the real tempo, the recording's feel is ambiguous; use your ears");

    const double pct = ctx.tempoHint > 0.0 ? (r.tempoBpm / ctx.tempoHint - 1.0) * 100.0 : 0.0;
    console("  project tempo at the item: " + fmt(ctx.tempoHint, 1) + " BPM; the recording is " + fmt(std::fabs(pct), 1) +
            "% " + (pct >= 0.0 ? "faster" : "slower") + ", so fitting it to the grid would stretch it by about " +
            fmt(std::fabs(pct), 0) + "%");

    if (!ValidatePtr2(ctx.proj, ctx.item, "MediaItem*")) return;
    const int tempoMarkers = CountTempoTimeSigMarkers(ctx.proj);
    if (tempoMarkers > 1) {
        console("  the project has a tempo map (" + std::to_string(tempoMarkers) +
                " tempo/time signature markers); not offering to change it. Edit the map by hand if the recording should set the tempo.");
        return;
    }

    // What a tempo change would do to everything else in the project.
    int follow = 0, fixed = 0;
    const int count = CountMediaItems(ctx.proj);
    for (int i = 0; i < count; ++i) {
        MediaItem* other = GetMediaItem(ctx.proj, i);
        if (!other || other == ctx.item) continue;
        (itemTimebase(ctx.proj, other) == 0 ? fixed : follow)++;
    }
    // The recording itself defines the tempo, so it must not be rescaled or
    // moved by the change. If its effective timebase follows the tempo, pin
    // it to Time first (otherwise REAPER would shrink or stretch it).
    const bool pinItem = itemTimebase(ctx.proj, ctx.item) != 0;
    const bool canSlide = !r.events.downbeats.empty();
    std::string q = "Set the project tempo to " + fmt(r.tempoBpm, 2) + " BPM (currently " + fmt(ctx.tempoHint, 2) + ")";
    q += canSlide ? ", and move \"" + ctx.label + "\" so its first downbeat sits on a bar line?\n\n"
                  : "?\n\n";
    if (pinItem) q += "\"" + ctx.label + "\" will be set to the Time base so it keeps its length" +
                      std::string(canSlide ? "" : " and position") + ".\n\n";
    if (follow + fixed == 0) {
        q += "No other items are affected.";
    } else {
        q += std::to_string(follow) + " other item(s) follow the tempo and will move or stretch; " + std::to_string(fixed) +
             " are on the Time base and stay where they are in seconds (but come off the grid).";
    }
    q += "\n\nUndo restores everything.";
    if (!askYesNo(q, "SRT: Detect tempo")) {
        console("  project tempo left at " + fmt(ctx.tempoHint, 1) + " BPM");
        return;
    }
    Undo_BeginBlock2(ctx.proj);
    if (pinItem) {
        SetMediaItemInfo_Value(ctx.item, "C_BEATATTACHMODE", 0.0);
        console("  \"" + ctx.label + "\" set to the Time base so the tempo change leaves it alone");
    }
    SetCurrentBPM(ctx.proj, r.tempoBpm, false);
    console("  project tempo set to " + fmt(r.tempoBpm, 2) + " BPM");

    if (canSlide) {
        // Slide the item (no stretching) so the first downbeat lands on the
        // target bar line, computed at the new tempo. Same bar rule as Fit,
        // including moving to a later bar when the lead-in would not fit
        // before the project start. The snap offset marks the downbeat so a
        // later hand nudge with snap on keeps it on the grid.
        const double firstItemTime = r.events.downbeats.front() / ctx.playrate;
        const int wanted = targetMeasureForBarOne(ctx.proj);
        int measure = wanted;
        double position = measureStartTime(ctx.proj, measure) - firstItemTime;
        while (position < 0.0 && measure - wanted < 64) position = measureStartTime(ctx.proj, ++measure) - firstItemTime;
        if (measure != wanted)
            console("  the " + fmt(firstItemTime, 2) + " s of audio before the first downbeat does not fit before bar " +
                    std::to_string(displayedBarForMeasure(ctx.proj, wanted)) + "; using bar " +
                    std::to_string(displayedBarForMeasure(ctx.proj, measure)) + " instead");
        SetMediaItemPosition(ctx.item, position, false);
        SetMediaItemInfo_Value(ctx.item, "D_SNAPOFFSET", firstItemTime);
        UpdateItemInProject(ctx.item);
        console("  moved \"" + ctx.label + "\" so its first downbeat is on bar " +
                std::to_string(displayedBarForMeasure(ctx.proj, measure)) + " (no stretching); snap offset set to that downbeat");
    } else {
        console("  no downbeat found, so the item was not moved");
    }
    Undo_EndBlock2(ctx.proj, "SRT: Set project tempo from recording", -1);
    UpdateArrange();
}

void runDetectWithModel(const std::filesystem::path& model) {
    const int count = CountSelectedMediaItems(nullptr);
    if (count == 0) {
        console("select a drum item first");
        return;
    }
    std::vector<MediaItem*> items;
    for (int i = 0; i < count; ++i) items.push_back(GetSelectedMediaItem(nullptr, i));
    for (MediaItem* item : items) {
        auto job = std::make_shared<DrumJob>();
        job->detectOnly = true;
        job->modelPath = model;
        if (!prepareItemForDetect(item, job->ctx)) continue;
        try {
            job->audio = readSourceAudio(job->ctx.take, job->ctx.srcStart, job->ctx.srcVisibleLength);
        } catch (const std::exception& ex) {
            console("skipped \"" + job->ctx.label + "\": " + ex.what());
            continue;
        }
        console("detecting the tempo of \"" + job->ctx.label + "\" (" + fmt(job->audio.durationSeconds(), 1) +
                " s of audio, wide search)");
        jobs().submit(job, &onDetectDone);
    }
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
void actionDetectTempo() {
    if (CountSelectedMediaItems(nullptr) == 0) {
        console("select a drum item first");
        return;
    }
    ensureModel(kBeatThisModel, [](const std::filesystem::path& model) { runDetectWithModel(model); });
}

} // namespace srt::reaper
