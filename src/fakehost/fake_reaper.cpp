#include "fake_reaper.h"

#include "srt/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace fake {

// A PCM_source backed by a WAV file loaded into memory.
class WavSource : public PCM_source {
public:
    std::string path;
    srt::AudioBuffer audio;
    explicit WavSource(const std::string& p) : path(p), audio(srt::readWav(p)) {}
    PCM_source* Duplicate() override { return new WavSource(path); }
    bool IsAvailable() override { return true; }
    const char* GetType() override { return "WAVE"; }
    const char* GetFileName() override { return path.c_str(); }
    bool SetFileName(const char*) override { return false; }
    int GetNumChannels() override { return static_cast<int>(audio.numChannels()); }
    double GetSampleRate() override { return audio.sampleRate; }
    double GetLength() override { return audio.durationSeconds(); }
    int PropertiesWindow(HWND) override { return 0; }
    void GetSamples(PCM_source_transfer_t* block) override {
        // Native rate only, as REAPER does when asked for the source's own rate.
        const long start = static_cast<long>(std::llround(block->time_s * audio.sampleRate));
        const long total = static_cast<long>(audio.numFrames());
        const int nch = block->nch;
        int out = 0;
        for (int f = 0; f < block->length; ++f) {
            const long idx = start + f;
            if (idx < 0 || idx >= total) break;
            for (int c = 0; c < nch; ++c) {
                const size_t ch = static_cast<size_t>(std::min<int>(c, GetNumChannels() - 1));
                block->samples[static_cast<size_t>(f) * nch + c] = audio.channels[ch][static_cast<size_t>(idx)];
            }
            ++out;
        }
        block->samples_out = out;
    }
    void GetPeakInfo(PCM_source_peaktransfer_t* block) override { block->peaks_out = 0; }
    void SaveState(ProjectStateContext*) override {}
    int LoadState(const char*, ProjectStateContext*) override { return -1; }
    void Peaks_Clear(bool) override {}
    int PeaksBuild_Begin() override { return 0; }
    int PeaksBuild_Run() override { return 0; }
    void PeaksBuild_Finish() override {}
};

Host& host() {
    static Host h;
    return h;
}

namespace {

Host& H() { return host(); }

std::string trimNewline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// ---- console, dialogs, misc ------------------------------------------------

void f_ShowConsoleMsg(const char* msg) {
    // The extension no longer writes to the ReaScript console (only the
    // too-old-REAPER message at load does); log lines arrive through
    // Host::logLine instead.
    std::string line = trimNewline(msg ? msg : "");
    H().console.push_back(line);
    if (H().echoConsole) std::printf("%s\n", line.c_str());
}
int f_ShowMessageBox(const char* msg, const char* title, int type) {
    std::printf("[messagebox type %d] %s: %s -> answer %d\n", type, title ? title : "", msg ? msg : "", H().messageBoxAnswer);
    H().messageBoxes.push_back(msg ? msg : "");
    return H().messageBoxAnswer;
}
HWND f_GetMainHwnd() { return nullptr; }
const char* f_GetResourcePath() { return H().project.resourcePath.c_str(); }
const char* f_GetAppVersion() { return "7.0/fakehost"; }
void f_Undo_BeginBlock2(ReaProject*) {}
void f_Undo_EndBlock2(ReaProject*, const char* desc, int) { H().undoBlocks.push_back(desc ? desc : ""); }
void f_UpdateArrange() {}
void f_UpdateItemInProject(MediaItem*) {}
void f_TrackList_AdjustWindows(bool) {}

int f_plugin_register(const char* name, void* info) {
    std::string n = name ? name : "";
    Host& h = H();
    if (n == "command_id") {
        const std::string id = static_cast<const char*>(info);
        auto it = h.commandIds.find(id);
        if (it != h.commandIds.end()) return it->second;
        const int cmd = 50000 + static_cast<int>(h.commandIds.size());
        h.commandIds[id] = cmd;
        return cmd;
    }
    if (n == "gaccel" || n == "-gaccel") return 1;
    if (n == "hookcommand") { h.hookCommand = reinterpret_cast<bool (*)(int, int)>(info); return 1; }
    if (n == "-hookcommand") { h.hookCommand = nullptr; return 1; }
    if (n == "timer") { h.timers.push_back(reinterpret_cast<void (*)()>(info)); return 1; }
    if (n == "-timer") {
        auto fn = reinterpret_cast<void (*)()>(info);
        h.timers.erase(std::remove(h.timers.begin(), h.timers.end(), fn), h.timers.end());
        return 1;
    }
    std::printf("[fakehost] plugin_register(\"%s\") not supported\n", n.c_str());
    return 0;
}

// ---- project queries -------------------------------------------------------

bool isItem(ReaProject* p, void* ptr) {
    for (auto& t : p->tracks)
        for (auto& i : t->items)
            if (i.get() == ptr) return true;
    return false;
}
bool isTake(ReaProject* p, void* ptr) {
    for (auto& t : p->tracks)
        for (auto& i : t->items)
            for (auto& k : i->takes)
                if (k.get() == ptr) return true;
    return false;
}
bool isTrack(ReaProject* p, void* ptr) {
    for (auto& t : p->tracks)
        if (t.get() == ptr) return true;
    return false;
}
ReaProject* proj(ReaProject* p) { return p ? p : &H().project; }

int f_CountSelectedMediaItems(ReaProject* p) {
    int n = 0;
    for (auto& t : proj(p)->tracks)
        for (auto& i : t->items) n += i->selected ? 1 : 0;
    return n;
}
int f_CountMediaItems(ReaProject* p) {
    int n = 0;
    for (auto& t : proj(p)->tracks) n += static_cast<int>(t->items.size());
    return n;
}
MediaItem* f_GetMediaItem(ReaProject* p, int idx) {
    int n = 0;
    for (auto& t : proj(p)->tracks)
        for (auto& i : t->items)
            if (n++ == idx) return i.get();
    return nullptr;
}
MediaTrack* f_GetMediaItem_Track(MediaItem* item) { return item->track; }
int f_CountTempoTimeSigMarkers(ReaProject* p) { return proj(p)->tempoMarkers; }
// Effective timebase of an item: its own, else its track's, else the project default.
int effectiveTimebase(ReaProject* p, MediaItem* item) {
    if (item->timebase >= 0) return item->timebase;
    if (item->track && item->track->timebase >= 0) return item->track->timebase;
    return proj(p)->itemTimebase;
}
// A tempo change moves and rescales items the way REAPER does: items on the
// Beats timebase keep their position in beats, so they move in seconds; with
// "position, length, rate" they are also stretched (length in seconds scales,
// playrate scales the other way). Items on the Time base stay put.
void f_SetCurrentBPM(ReaProject* p, double bpm, bool) {
    ReaProject* P = proj(p);
    const double ratio = P->bpm / bpm; // seconds per beat, new over old
    for (auto& t : P->tracks)
        for (auto& i : t->items) {
            const int tb = effectiveTimebase(P, i.get());
            if (tb == 0) continue;
            i->position *= ratio;
            if (tb == 1) {
                i->length *= ratio;
                for (auto& take : i->takes) take->playrate /= ratio;
            }
        }
    P->bpm = bpm;
}
// Project config variables: only the item timebase is modelled.
int f_projectconfig_var_getoffs(const char* name, int* szOut) {
    if (name && std::strcmp(name, "itemtimelock") == 0) {
        if (szOut) *szOut = static_cast<int>(sizeof(int));
        return 1;
    }
    return -1;
}
void* f_projectconfig_var_addr(ReaProject* p, int idx) { return idx == 1 ? &proj(p)->itemTimebase : nullptr; }
MediaItem* f_GetSelectedMediaItem(ReaProject* p, int sel) {
    int n = 0;
    for (auto& t : proj(p)->tracks)
        for (auto& i : t->items)
            if (i->selected && n++ == sel) return i.get();
    return nullptr;
}
MediaItem_Take* f_GetActiveTake(MediaItem* item) { return item ? item->activeTake() : nullptr; }
const char* f_GetTakeName(MediaItem_Take* take) { return take ? take->name.c_str() : nullptr; }
PCM_source* f_GetMediaItemTake_Source(MediaItem_Take* take) { return take ? take->source : nullptr; }
MediaItem* f_GetMediaItemTake_Item(MediaItem_Take* take) { return take ? take->item : nullptr; }
MediaTrack* f_GetMediaItemTrack(MediaItem* item) { return item ? item->track : nullptr; }
ReaProject* f_GetItemProjectContext(MediaItem*) { return &H().project; }
bool f_ValidatePtr2(ReaProject* p, void* ptr, const char* type) {
    std::string t = type ? type : "";
    if (t == "MediaItem*") return isItem(proj(p), ptr);
    if (t == "MediaItem_Take*") return isTake(proj(p), ptr);
    if (t == "MediaTrack*") return isTrack(proj(p), ptr);
    return ptr != nullptr;
}

double f_GetMediaItemInfo_Value(MediaItem* item, const char* parm) {
    std::string p = parm ? parm : "";
    if (p == "D_POSITION") return item->position;
    if (p == "D_LENGTH") return item->length;
    if (p == "C_BEATATTACHMODE") return item->timebase;
    if (p == "D_SNAPOFFSET") return item->snapOffset;
    std::printf("[fakehost] GetMediaItemInfo_Value(%s) unsupported\n", p.c_str());
    return 0.0;
}
bool f_SetMediaItemInfo_Value(MediaItem* item, const char* parm, double v) {
    std::string p = parm ? parm : "";
    if (p == "D_POSITION") { item->position = v; return true; }
    if (p == "D_LENGTH") { item->length = v; return true; }
    if (p == "C_BEATATTACHMODE") { item->timebase = static_cast<int>(v); return true; }
    if (p == "D_SNAPOFFSET") { item->snapOffset = v; return true; }
    std::printf("[fakehost] SetMediaItemInfo_Value(%s) unsupported\n", p.c_str());
    return false;
}
// REAPER never places an item before the project start: a negative position
// is clamped to 0, which silently shifts everything the caller planned.
bool f_SetMediaItemPosition(MediaItem* item, double pos, bool) {
    if (pos < 0.0) {
        std::printf("[fakehost] SetMediaItemPosition(%.4f) clamped to 0 as REAPER would\n", pos);
        pos = 0.0;
    }
    item->position = pos;
    return true;
}
bool f_SetMediaItemLength(MediaItem* item, double len, bool) { item->length = len; return true; }
double f_GetMediaItemTakeInfo_Value(MediaItem_Take* take, const char* parm) {
    std::string p = parm ? parm : "";
    if (p == "D_STARTOFFS") return take->startOffset;
    if (p == "D_PLAYRATE") return take->playrate;
    std::printf("[fakehost] GetMediaItemTakeInfo_Value(%s) unsupported\n", p.c_str());
    return 0.0;
}
bool f_SetMediaItemTakeInfo_Value(MediaItem_Take* take, const char* parm, double v) {
    std::string p = parm ? parm : "";
    if (p == "D_STARTOFFS") { take->startOffset = v; return true; }
    if (p == "D_PLAYRATE") { take->playrate = v; return true; }
    std::printf("[fakehost] SetMediaItemTakeInfo_Value(%s) unsupported\n", p.c_str());
    return false;
}
bool f_GetSetMediaItemInfo_String(MediaItem* item, const char* parm, char* buf, bool set) {
    std::string p = parm ? parm : "";
    if (p.rfind("P_EXT:", 0) == 0) {
        if (set) item->ext[p] = buf ? buf : "";
        else {
            auto it = item->ext.find(p);
            if (it == item->ext.end()) { buf[0] = 0; return false; }
            std::strcpy(buf, it->second.c_str());
        }
        return true;
    }
    std::printf("[fakehost] GetSetMediaItemInfo_String(%s) unsupported\n", p.c_str());
    return false;
}
bool f_GetSetMediaItemTakeInfo_String(MediaItem_Take* take, const char* parm, char* buf, bool set) {
    std::string p = parm ? parm : "";
    if (p == "P_NAME") {
        if (set) take->name = buf ? buf : "";
        else std::strcpy(buf, take->name.c_str());
        return true;
    }
    std::printf("[fakehost] GetSetMediaItemTakeInfo_String(%s) unsupported\n", p.c_str());
    return false;
}

// ---- sources ---------------------------------------------------------------

void f_GetMediaSourceType(PCM_source* src, char* buf, int sz) {
    std::snprintf(buf, static_cast<size_t>(sz), "%s", src ? src->GetType() : "");
}
double f_GetMediaSourceLength(PCM_source* src, bool* isQn) {
    if (isQn) *isQn = false;
    return src ? src->GetLength() : 0.0;
}
int f_GetMediaSourceSampleRate(PCM_source* src) { return src ? static_cast<int>(src->GetSampleRate()) : 0; }
int f_GetMediaSourceNumChannels(PCM_source* src) { return src ? src->GetNumChannels() : 0; }
PCM_source* f_PCM_Source_CreateFromFile(const char* path) {
    try {
        return new WavSource(path);
    } catch (const std::exception& ex) {
        std::printf("[fakehost] cannot open %s: %s\n", path, ex.what());
        return nullptr;
    }
}
bool f_SetMediaItemTake_Source(MediaItem_Take* take, PCM_source* src) { take->source = src; return true; }

// ---- stretch markers -------------------------------------------------------
// pos is item time, srcpos is source time. Markers are kept sorted by pos.
// Adding (idx < 0) with a srcpos inserts and returns the new index; adding
// without srcpos derives it linearly from the neighbours (or playrate).

int f_GetTakeNumStretchMarkers(MediaItem_Take* take) { return static_cast<int>(take->markers.size()); }
int f_GetTakeStretchMarker(MediaItem_Take* take, int idx, double* pos, double* src) {
    if (idx < 0 || idx >= static_cast<int>(take->markers.size())) return -1;
    if (pos) *pos = take->markers[static_cast<size_t>(idx)].pos;
    if (src) *src = take->markers[static_cast<size_t>(idx)].srcpos;
    return idx;
}
int f_SetTakeStretchMarker(MediaItem_Take* take, int idx, double pos, const double* src) {
    auto& m = take->markers;
    if (idx >= 0) {
        if (idx >= static_cast<int>(m.size())) return -1;
        m[static_cast<size_t>(idx)].pos = pos;
        if (src) m[static_cast<size_t>(idx)].srcpos = *src;
        return idx;
    }
    for (const auto& e : m)
        if (std::fabs(e.pos - pos) < 1e-9) return -1; // already exists at that time
    double srcpos = src ? *src : take->startOffset + pos * take->playrate;
    StretchMarker nm{pos, srcpos};
    auto it = std::lower_bound(m.begin(), m.end(), pos, [](const StretchMarker& a, double p) { return a.pos < p; });
    it = m.insert(it, nm);
    return static_cast<int>(it - m.begin());
}
int f_DeleteTakeStretchMarkers(MediaItem_Take* take, int idx, const int* count) {
    auto& m = take->markers;
    if (idx < 0 || idx >= static_cast<int>(m.size())) return 0;
    const int n = std::min<int>(count ? *count : 1, static_cast<int>(m.size()) - idx);
    m.erase(m.begin() + idx, m.begin() + idx + n);
    return n;
}

// ---- tempo map (constant tempo, 4/4) ----------------------------------------

double beatSeconds() { return 60.0 / H().project.bpm; }
double barSeconds() { return beatSeconds() * H().project.beatsPerBar; }

double f_TimeMap2_beatsToTime(ReaProject*, double beats, const int* measure) {
    if (measure) return *measure * barSeconds() + beats * beatSeconds();
    return beats * beatSeconds();
}
double f_TimeMap_GetMeasureInfo(ReaProject*, int measure, double* qnStart, double* qnEnd, int* num, int* denom, double* tempo) {
    const auto& p = H().project;
    if (qnStart) *qnStart = measure * p.beatsPerBar;
    if (qnEnd) *qnEnd = (measure + 1) * p.beatsPerBar;
    if (num) *num = p.beatsPerBar;
    if (denom) *denom = 4;
    if (tempo) *tempo = p.bpm;
    return measure * barSeconds();
}
double f_TimeMap_GetDividedBpmAtTime(double) { return H().project.bpm; }
void f_format_timestr_pos(double t, char* buf, int sz, int mode) {
    const auto& p = H().project;
    if (mode == 2 || mode == 1 || mode == -1) {
        const int measure = static_cast<int>(std::floor(t / barSeconds()));
        const double inBar = t - measure * barSeconds();
        const int beat = static_cast<int>(std::floor(inBar / beatSeconds()));
        const double frac = (inBar - beat * beatSeconds()) / beatSeconds();
        std::snprintf(buf, static_cast<size_t>(sz), "%d.%d.%02d", measure + p.startBar, beat + 1, static_cast<int>(frac * 100));
    } else {
        std::snprintf(buf, static_cast<size_t>(sz), "%.3f", t);
    }
}

// ---- tracks ----------------------------------------------------------------

void f_InsertTrackAtIndex(int idx, bool) {
    auto& tracks = H().project.tracks;
    idx = std::clamp<int>(idx, 0, static_cast<int>(tracks.size()));
    tracks.insert(tracks.begin() + idx, std::make_unique<MediaTrack>());
}
MediaTrack* f_GetTrack(ReaProject* p, int idx) {
    auto& tracks = proj(p)->tracks;
    return idx >= 0 && idx < static_cast<int>(tracks.size()) ? tracks[static_cast<size_t>(idx)].get() : nullptr;
}
double f_GetMediaTrackInfo_Value(MediaTrack* tr, const char* parm) {
    std::string p = parm ? parm : "";
    if (p == "C_BEATATTACHMODE") return tr->timebase;
    if (p == "IP_TRACKNUMBER") {
        auto& tracks = H().project.tracks;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].get() == tr) return static_cast<double>(i + 1);
        return 0.0;
    }
    if (p == "I_FOLDERDEPTH") return tr->folderDepth;
    std::printf("[fakehost] GetMediaTrackInfo_Value(%s) unsupported\n", p.c_str());
    return 0.0;
}
bool f_SetMediaTrackInfo_Value(MediaTrack* tr, const char* parm, double v) {
    std::string p = parm ? parm : "";
    if (p == "I_FOLDERDEPTH") { tr->folderDepth = static_cast<int>(v); return true; }
    std::printf("[fakehost] SetMediaTrackInfo_Value(%s) unsupported\n", p.c_str());
    return false;
}
bool f_GetSetMediaTrackInfo_String(MediaTrack* tr, const char* parm, char* buf, bool set) {
    std::string p = parm ? parm : "";
    if (p == "P_NAME") {
        if (set) tr->name = buf ? buf : "";
        else std::strcpy(buf, tr->name.c_str());
        return true;
    }
    std::printf("[fakehost] GetSetMediaTrackInfo_String(%s) unsupported\n", p.c_str());
    return false;
}
MediaItem* f_AddMediaItemToTrack(MediaTrack* tr) {
    auto item = std::make_unique<MediaItem>();
    item->track = tr;
    tr->items.push_back(std::move(item));
    return tr->items.back().get();
}
MediaItem_Take* f_AddTakeToMediaItem(MediaItem* item) {
    auto take = std::make_unique<MediaItem_Take>();
    take->item = item;
    item->takes.push_back(std::move(take));
    return item->takes.back().get();
}
void f_GetProjectPathEx(ReaProject* p, char* buf, int sz) { std::snprintf(buf, static_cast<size_t>(sz), "%s", proj(p)->mediaDir.c_str()); }

// ---- the function table ----------------------------------------------------
// Each entry is checked at compile time against the extension's own
// declaration of the function pointer, so a signature mismatch cannot slip in.

struct Entry {
    const char* name;
    void* fn;
};

template <typename Declared, typename Impl>
void* checked(Impl impl) {
    static_assert(std::is_same<Declared, Impl>::value, "fake REAPER function has the wrong signature");
    return reinterpret_cast<void*>(impl);
}
#define FAKE(name) Entry{#name, checked<decltype(name)>(&f_##name)}

const Entry kTable[] = {
    FAKE(ShowConsoleMsg), FAKE(ShowMessageBox), FAKE(GetMainHwnd), FAKE(GetResourcePath), FAKE(GetAppVersion),
    FAKE(plugin_register), FAKE(Undo_BeginBlock2), FAKE(Undo_EndBlock2), FAKE(UpdateArrange),
    FAKE(CountSelectedMediaItems), FAKE(GetSelectedMediaItem), FAKE(GetActiveTake), FAKE(GetTakeName),
    FAKE(CountMediaItems), FAKE(GetMediaItem), FAKE(GetMediaItem_Track), FAKE(CountTempoTimeSigMarkers),
    FAKE(SetCurrentBPM), FAKE(projectconfig_var_getoffs), FAKE(projectconfig_var_addr),
    FAKE(GetMediaItemTake_Source), FAKE(GetMediaItemTake_Item), FAKE(GetMediaItemTrack), FAKE(GetMediaItemInfo_Value),
    FAKE(SetMediaItemInfo_Value), FAKE(GetMediaItemTakeInfo_Value), FAKE(GetSetMediaItemInfo_String),
    FAKE(SetMediaItemPosition), FAKE(SetMediaItemLength), FAKE(UpdateItemInProject), FAKE(GetItemProjectContext),
    FAKE(ValidatePtr2), FAKE(GetMediaSourceType), FAKE(GetMediaSourceLength), FAKE(GetMediaSourceSampleRate),
    FAKE(GetMediaSourceNumChannels), FAKE(GetTakeNumStretchMarkers), FAKE(GetTakeStretchMarker),
    FAKE(SetTakeStretchMarker), FAKE(DeleteTakeStretchMarkers), FAKE(TimeMap2_beatsToTime),
    FAKE(TimeMap_GetMeasureInfo), FAKE(TimeMap_GetDividedBpmAtTime), FAKE(format_timestr_pos),
    FAKE(InsertTrackAtIndex), FAKE(GetTrack), FAKE(GetMediaTrackInfo_Value), FAKE(SetMediaTrackInfo_Value),
    FAKE(GetSetMediaTrackInfo_String), FAKE(AddMediaItemToTrack), FAKE(AddTakeToMediaItem),
    FAKE(PCM_Source_CreateFromFile), FAKE(SetMediaItemTake_Source), FAKE(SetMediaItemTakeInfo_Value),
    FAKE(GetSetMediaItemTakeInfo_String), FAKE(GetProjectPathEx), FAKE(TrackList_AdjustWindows),
};

void* getFunc(const char* name) {
    for (const auto& e : kTable)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    std::printf("[fakehost] extension asked for unsupported API function %s\n", name);
    return nullptr;
}

int registerFn(const char* name, void* info) { return f_plugin_register(name, info); }

} // namespace

reaper_plugin_info_t Host::pluginInfo() {
    reaper_plugin_info_t rec{};
    rec.caller_version = REAPER_PLUGIN_VERSION;
    rec.hwnd_main = nullptr;
    rec.Register = &registerFn;
    rec.GetFunc = &getFunc;
    return rec;
}

int Host::commandId(const std::string& idString) const {
    auto it = commandIds.find(idString);
    return it == commandIds.end() ? 0 : it->second;
}

bool Host::runCommand(const std::string& idString) {
    const int cmd = commandId(idString);
    if (!cmd || !hookCommand) return false;
    return hookCommand(cmd, 0);
}

void Host::tick() {
    auto copy = timers;
    for (auto fn : copy) fn();
}

MediaTrack* Host::addTrack(const std::string& name) {
    project.tracks.push_back(std::make_unique<MediaTrack>());
    project.tracks.back()->name = name;
    return project.tracks.back().get();
}

MediaItem* Host::addItem(MediaTrack* track, const std::string& takeName, const std::string& wavPath, double position) {
    auto* src = new WavSource(wavPath);
    auto item = std::make_unique<MediaItem>();
    item->track = track;
    item->position = position;
    item->length = src->GetLength();
    auto take = std::make_unique<MediaItem_Take>();
    take->item = item.get();
    take->name = takeName;
    take->source = src;
    item->takes.push_back(std::move(take));
    track->items.push_back(std::move(item));
    return track->items.back().get();
}

void Host::logLine(const std::string& stamped) {
    // Lines come as "HH:MM:SS SRT: ..."; keep the part after the timestamp so
    // the scenarios match on "SRT: ..." as before.
    std::string line = stamped.size() > 9 && stamped[8] == ' ' ? stamped.substr(9) : stamped;
    console.push_back(line);
    if (echoConsole) std::printf("%s\n", line.c_str());
}

double Host::barTime(int displayedBar) const {
    const int measure = displayedBar - project.startBar;
    return measure * (60.0 / project.bpm) * project.beatsPerBar;
}

std::vector<std::string> Host::dump() const {
    std::vector<std::string> out;
    char buf[512];
    for (size_t t = 0; t < project.tracks.size(); ++t) {
        const auto& tr = project.tracks[t];
        std::snprintf(buf, sizeof(buf), "track %zu \"%s\" depth %d", t + 1, tr->name.c_str(), tr->folderDepth);
        out.push_back(buf);
        for (const auto& item : tr->items) {
            const auto* take = item->activeTake();
            std::snprintf(buf, sizeof(buf), "  item pos %.4f len %.4f snapoffs %.4f take \"%s\" startoffs %.4f rate %.3f source %s",
                          item->position, item->length, item->snapOffset, take ? take->name.c_str() : "", take ? take->startOffset : 0.0,
                          take ? take->playrate : 1.0,
                          take && take->source ? (take->source->GetFileName() ? take->source->GetFileName() : "?") : "none");
            out.push_back(buf);
            if (take)
                for (size_t m = 0; m < take->markers.size(); ++m) {
                    std::snprintf(buf, sizeof(buf), "    marker %zu pos %.4f (project %.4f) src %.4f", m,
                                  take->markers[m].pos, item->position + take->markers[m].pos, take->markers[m].srcpos);
                    out.push_back(buf);
                }
            for (const auto& [k, v] : item->ext) {
                std::snprintf(buf, sizeof(buf), "    %s = %s", k.c_str(), v.c_str());
                out.push_back(buf);
            }
        }
    }
    return out;
}

} // namespace fake
