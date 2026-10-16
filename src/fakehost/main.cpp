// srt_fakehost: loads the extension the way REAPER does (ReaperPluginEntry),
// builds a small project around a WAV file, runs an SRT action by its command
// ID, pumps the timer until the background job finishes, and checks the
// result. Exit codes: 0 pass, 1 fail, 2 usage, 77 skipped (model missing).
//
//   srt_fakehost about
//   srt_fakehost fit <drums.wav> [--bpm 120] [--start-bar -2] [--expect-bars N] [--position SEC]
//   srt_fakehost conform <drums.wav> [same options]
//   srt_fakehost tempo <drums.wav> [--bpm 120] [--expect-bpm 121.3] [--answer 6|7]
//   srt_fakehost separate <mix.wav>
//   srt_fakehost shell <wav>          (set up a project, run commands typed on stdin)
//
// Add --plugin <path to reaper_srt.dll> before the mode to load a separately
// built extension from disk the way REAPER does (LoadLibrary + the exported
// entry point) instead of the copy linked into this program. On Windows (or
// under Wine) that exercises a CI-built or locally cross-built DLL as-is,
// including its own runtime loading, dialogs and jobs window.

#include "fake_reaper.h"
#include "host.h"
#include "models.h"
#include "srt/wav.h"

#include <algorithm>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec);

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

// Folders are found relative to this program, which sits at
// <build>/src/fakehost (single-config generators) or
// <build>/src/fakehost/<Config> (Visual Studio, Xcode): the models the unit
// tests download live in <build>/_models and the fake project's files go
// under <build>/fakehost. The build folder is the nearest ancestor that holds
// "_models". (Relative to the program rather than baked in at build time so
// the same binary works under Wine, where the build folder has a Windows
// path.)
fs::path g_exeDir;
fs::path buildRootDir() {
    fs::path dir = g_exeDir;
    for (int up = 0; up < 4 && !dir.empty(); ++up) {
        if (fs::is_directory(dir / "_models")) return dir;
        dir = dir.parent_path();
    }
    return g_exeDir / ".." / "..";
}
fs::path modelsCacheDir() { return buildRootDir() / "_models"; }
fs::path workRootDir() { return buildRootDir() / "fakehost"; }

// The entry point to drive: ours (linked in) unless --plugin names a library.
using EntryFn = int (*)(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t*);
EntryFn g_entry = &REAPER_PLUGIN_ENTRYPOINT;
REAPER_PLUGIN_HINSTANCE g_instance = nullptr;
bool g_external = false;

bool loadPluginLibrary(const std::string& path) {
#ifdef _WIN32
    const std::wstring wpath = fs::absolute(path).wstring();
    HMODULE m = LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        std::printf("could not load %s (Windows error %lu)\n", path.c_str(), GetLastError());
        return false;
    }
    auto fn = reinterpret_cast<EntryFn>(reinterpret_cast<void*>(GetProcAddress(m, "ReaperPluginEntry")));
    g_instance = m;
#else
    void* m = dlopen(fs::absolute(path).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        std::printf("could not load %s: %s\n", path.c_str(), dlerror());
        return false;
    }
    auto fn = reinterpret_cast<EntryFn>(dlsym(m, "ReaperPluginEntry"));
    g_instance = m;
    std::printf("note: on this platform the extension's window code expects REAPER's SWELL; "
                "scenarios that open the jobs window will not work with --plugin\n");
#endif
    if (!fn) {
        std::printf("%s does not export ReaperPluginEntry\n", path.c_str());
        return false;
    }
    g_entry = fn;
    g_external = true;
    std::printf("loaded extension from %s\n", path.c_str());
    return true;
}

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) ++g_failures;
}

bool askedContains(const std::string& needle) {
    for (const auto& q : fake::host().messageBoxes)
        if (q.find(needle) != std::string::npos) return true;
    return false;
}

// With --plugin the extension's own log window code runs (no headless
// replacement), so its lines are read from the log file it writes in the
// fake project's resource folder, as they appear.
std::streamoff g_logOffset = 0;
fs::path logFile() { return fs::path(fake::host().project.resourcePath) / "SRT" / "srt.log"; }
void markLogStart() {
    std::error_code ec;
    const auto size = fs::file_size(logFile(), ec);
    g_logOffset = ec ? 0 : static_cast<std::streamoff>(size);
}
void pollLogFile() {
    if (!g_external) return;
    std::ifstream f(logFile(), std::ios::binary);
    if (!f) return;
    f.seekg(g_logOffset);
    std::string rest((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Only complete lines; a partial last line waits for its newline.
    size_t pos = 0;
    for (;;) {
        const size_t nl = rest.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = rest.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) fake::host().logLine(line);
        pos = nl + 1;
    }
    g_logOffset += static_cast<std::streamoff>(pos);
}

bool consoleContains(const std::string& needle) {
    for (const auto& line : fake::host().console)
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

// Mirrors <resource>/SRT/models/<id>/<file> onto the test model cache that the
// unit tests download. Returns false if the cached model is not there.
bool installModel(const srt::reaper::ModelSpec& spec, const fs::path& resource) {
    // The Demucs exports live in a subfolder (tools/export_demucs.py writes there).
    fs::path cached = modelsCacheDir() / spec.fileName;
    if (!fs::exists(cached)) cached = modelsCacheDir() / "demucs" / spec.fileName;
    if (!fs::exists(cached)) {
        std::printf("model %s not found at %s\n", spec.fileName, (modelsCacheDir() / spec.fileName).string().c_str());
        return false;
    }
    const fs::path dest = resource / "SRT" / "models" / spec.id / spec.fileName;
    fs::create_directories(dest.parent_path());
    if (!fs::exists(dest)) {
        std::error_code ec;
        fs::create_symlink(fs::absolute(cached), dest, ec);
        if (ec) fs::copy_file(cached, dest, fs::copy_options::overwrite_existing);
    }
    return true;
}

// Fresh work directory with a media folder and a resource folder.
void prepareWorkDir(fs::path& media, fs::path& resource) {
    const fs::path work = workRootDir();
    media = work / "media";
    resource = work / "resource";
    std::error_code ec;
    fs::remove_all(media, ec);
    fs::create_directories(media);
    fs::create_directories(resource);
    auto& p = fake::host().project;
    p.mediaDir = media.string();
    p.resourcePath = resource.string();
}

bool loadExtension() {
    markLogStart();
    reaper_plugin_info_t rec = fake::host().pluginInfo();
    const int rc = g_entry(g_instance, &rec);
    pollLogFile(); // the "loaded version" line, when the extension is external
    check(rc == 1, "ReaperPluginEntry returned 1");
    check(fake::host().commandId("SRT_ABOUT") != 0, "SRT_ABOUT registered");
    check(fake::host().commandId("SRT_SHOW_LOG") != 0, "SRT_SHOW_LOG registered");
    check(fake::host().commandId("SRT_FIT_DRUMS_TO_GRID") != 0, "SRT_FIT_DRUMS_TO_GRID registered");
    check(fake::host().commandId("SRT_CONFORM_DRUM_BARS") != 0, "SRT_CONFORM_DRUM_BARS registered");
    check(fake::host().commandId("SRT_SEPARATE_STEMS") != 0, "SRT_SEPARATE_STEMS registered");
    check(!fake::host().timers.empty(), "timer registered");
    return rc == 1;
}

void unloadExtension() { g_entry(g_instance, nullptr); }

// Runs the command and pumps the main-thread timer until the action is
// finished. "Finished" is read from the console: every action ends with a
// known line (success, nothing to do, or an error). With the extension linked
// in we also wait for its job runner to go idle; a separately loaded library
// has its own runner we cannot see, so the console is all we have there.
void runAndWait(const std::string& idString, double timeoutSeconds = 1800.0) {
    static const char* const kDoneMarkers[] = {
        // about
        "background check OK", "background check cancelled", "background check FAILED", "about: FAILED",
        // fit / conform
        "fitted ", "conformed ", "could not find two downbeats", "nothing applied", "is gone;", "not applied:",
        // detect tempo
        "project tempo set to", "project tempo left at", "no tempo found", "tempo map (",
        // separate
        "separated \"", "failed on \"", "cancelled \"",
        // shared
        "model download declined", "model download failed", "model download cancelled", "cannot download",
        "select at least one", "  error: ",
    };
    const size_t consoleBefore = fake::host().console.size();
    auto finished = [&] {
        for (size_t i = consoleBefore; i < fake::host().console.size(); ++i)
            for (const char* m : kDoneMarkers)
                if (fake::host().console[i].find(m) != std::string::npos) return true;
        return false;
    };
    // Items the action skipped before starting any job also count as finished.
    auto skippedEverything = [&] {
        bool sawSkip = false;
        for (size_t i = consoleBefore; i < fake::host().console.size(); ++i)
            if (fake::host().console[i].find("skipped ") != std::string::npos) sawSkip = true;
        return sawSkip && !g_external && !srt::reaper::jobs().hasActive();
    };

    std::printf("-- running %s\n", idString.c_str());
    const bool handled = fake::host().runCommand(idString);
    check(handled, "command handled by hookcommand");
    const auto start = std::chrono::steady_clock::now();
    int idleTicks = 0;
    for (;;) {
        fake::host().tick();
        pollLogFile();
        const bool busy = !g_external && srt::reaper::jobs().hasActive();
        if (busy) {
            idleTicks = 0;
        } else if ((finished() || skippedEverything()) && ++idleTicks >= 5) {
            break; // a few quiet ticks so continuations get to run
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeoutSeconds) {
            check(false, "timed out waiting for the action to finish");
            if (!g_external) srt::reaper::jobs().cancelAll();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void dumpProject() {
    for (const auto& line : fake::host().dump()) std::printf("%s\n", line.c_str());
}

struct DrumOptions {
    std::string wav;
    double bpm = 120.0;
    int startBar = -2;
    int expectBars = -1;
    double position = 3.0;
    double expectBpm = 0.0;
    int answer = 6; // message box answer: 6 = yes, 7 = no
    double pad = 0.0; // seconds of silence to prepend to the recording (a long lead-in)
    int expectFirstBar = 0; // bar the first marker must land on; 0 = bar 1 with lead-in bars, else bar 2
};

bool parseDrumOptions(int argc, char** argv, DrumOptions& o) {
    if (argc < 3) return false;
    o.wav = argv[2];
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--bpm") o.bpm = std::atof(v);
        else if (k == "--start-bar") o.startBar = std::atoi(v);
        else if (k == "--expect-bars") o.expectBars = std::atoi(v);
        else if (k == "--position") o.position = std::atof(v);
        else if (k == "--expect-bpm") o.expectBpm = std::atof(v);
        else if (k == "--answer") o.answer = std::atoi(v);
        else if (k == "--pad") o.pad = std::atof(v);
        else if (k == "--expect-first-bar") o.expectFirstBar = std::atoi(v);
        else return false;
    }
    return true;
}

// Project with one track and one selected drum item.
MediaItem* buildDrumProject(const DrumOptions& o) {
    auto& h = fake::host();
    h.project.bpm = o.bpm;
    h.project.startBar = o.startBar;
    MediaTrack* tr = h.addTrack("Drums");
    std::string wav = o.wav;
    if (o.pad > 0.0) {
        // A copy with silence in front, in the fake project's media folder.
        srt::AudioBuffer src = srt::readWav(o.wav);
        const size_t extra = static_cast<size_t>(o.pad * src.sampleRate);
        srt::AudioBuffer padded(src.numChannels(), src.numFrames() + extra, src.sampleRate);
        for (size_t c = 0; c < src.numChannels(); ++c)
            std::copy(src.channels[c].begin(), src.channels[c].end(), padded.channels[c].begin() + static_cast<long>(extra));
        const fs::path out = fs::path(h.project.mediaDir) / (fs::path(o.wav).stem().string() + "_padded.wav");
        srt::writeWav(out, padded);
        wav = out.string();
    }
    MediaItem* item = h.addItem(tr, fs::path(o.wav).stem().string(), wav, o.position);
    item->selected = true;
    return item;
}

int runDrums(int argc, char** argv, bool conform) {
    DrumOptions o;
    if (!parseDrumOptions(argc, argv, o)) return 2;
    fs::path media, resource;
    prepareWorkDir(media, resource);
    if (!installModel(srt::reaper::kBeatThisModel, resource)) return 77;
    if (!loadExtension()) return 1;
    MediaItem* item = buildDrumProject(o);
    const double before = item->position;

    runAndWait(conform ? "SRT_CONFORM_DRUM_BARS" : "SRT_FIT_DRUMS_TO_GRID");
    dumpProject();

    auto& h = fake::host();
    MediaItem_Take* take = item->activeTake();
    // Bar 1 if the project has bars before it, otherwise bar 2; or later when
    // the lead-in would otherwise push the item before the project start.
    const int firstBar = o.expectFirstBar > 0 ? o.expectFirstBar : o.startBar < 1 ? 1 : 2;
    const double tol = 0.005;
    const auto& m = take->markers;
    check(item->position != before, "item was moved");
    check(item->position >= 0.0, "item starts at or after the project start (" + std::to_string(item->position) + " s)");
    check(!consoleContains("clamped to 0"), "the action never asked for a negative item position");
    if (o.expectFirstBar > 0)
        check(consoleContains("does not fit before bar") && consoleContains("using bar " + std::to_string(o.expectFirstBar)),
              "console explains the later bar");
    check(m.size() >= 2, "at least two stretch markers");
    if (m.size() >= 2) {
        const double firstProj = item->position + m.front().pos;
        const double lastProj = item->position + m.back().pos;
        check(std::fabs(firstProj - h.barTime(firstBar)) < tol,
              "first marker on bar " + std::to_string(firstBar) + " (" + std::to_string(firstProj) + " s)");
        check(std::fabs(item->snapOffset - m.front().pos) < 1e-9, "snap offset on the first marker");
        auto barsIt = item->ext.find("P_EXT:SRT_BARS");
        check(barsIt != item->ext.end(), "P_EXT:SRT_BARS stored");
        int bars = barsIt != item->ext.end() ? std::atoi(barsIt->second.c_str()) : 0;
        if (o.expectBars > 0) check(bars == o.expectBars, "bar count " + std::to_string(bars) + " == " + std::to_string(o.expectBars));
        check(std::fabs(lastProj - h.barTime(firstBar + bars)) < tol,
              "last marker on bar " + std::to_string(firstBar + bars) + " (" + std::to_string(lastProj) + " s)");
        if (conform) {
            check(static_cast<int>(m.size()) == bars + 1, "one marker per bar line (" + std::to_string(m.size()) + ")");
            for (size_t i = 0; i < m.size(); ++i) {
                const double proj = item->position + m[i].pos;
                const double bar = h.barTime(firstBar + static_cast<int>(i));
                check(std::fabs(proj - bar) < tol, "marker " + std::to_string(i) + " at " + std::to_string(proj) + " s, bar line " + std::to_string(bar));
            }
            for (size_t i = 1; i < m.size(); ++i)
                check(m[i].srcpos > m[i - 1].srcpos && m[i].pos > m[i - 1].pos, "marker " + std::to_string(i) + " keeps order");
        } else {
            check(m.size() == 2, "exactly two stretch markers");
        }
    }
    check(h.undoBlocks.size() == 1, "one undo block (" + std::to_string(h.undoBlocks.size()) + ")");
    check(!consoleContains("error:"), "no errors in console");
    unloadExtension();
    return g_failures ? 1 : 0;
}

// "Detect tempo": report, then (with answer 6) set the project tempo.
int runTempo(int argc, char** argv) {
    DrumOptions o;
    if (!parseDrumOptions(argc, argv, o)) return 2;
    fs::path media, resource;
    prepareWorkDir(media, resource);
    if (!installModel(srt::reaper::kBeatThisModel, resource)) return 77;
    if (!loadExtension()) return 1;
    auto& h = fake::host();
    MediaItem* item = buildDrumProject(o);
    // Two bystander items: one follows the tempo (project default), one is on the Time base.
    MediaTrack* other = h.addTrack("Other");
    MediaItem* follows = h.addItem(other, "follows", o.wav, 30.0);
    h.addItem(other, "fixed", o.wav, 50.0)->timebase = 0;
    h.messageBoxAnswer = o.answer;
    const double itemLength = item->length;
    const double itemRate = item->activeTake()->playrate;
    const double followsLength = follows->length;

    runAndWait("SRT_DETECT_TEMPO");
    dumpProject();

    check(consoleContains("BPM detected"), "tempo reported");
    check(askedContains("1 other item(s) follow the tempo and will move or stretch; 1 are on the Time base"),
          "consequences for the other items spelled out in the question");
    check(item->activeTake()->markers.empty(), "no stretch markers added");
    check(std::fabs(item->length - itemLength) < 1e-9 && std::fabs(item->activeTake()->playrate - itemRate) < 1e-9,
          "the item keeps its length and playrate");
    if (o.answer == 6) {
        check(consoleContains("project tempo set to"), "project tempo set");
        if (o.expectBpm > 0.0)
            check(std::fabs(h.project.bpm - o.expectBpm) < 1.5,
                  "project tempo " + std::to_string(h.project.bpm) + " near " + std::to_string(o.expectBpm));
        check(item->timebase == 0, "the item was pinned to the Time base");
        check(askedContains("will be set to the Time base"), "the question says the item will be pinned");
        check(askedContains("move \"" + item->activeTake()->name + "\" so its first downbeat sits on a bar line"),
              "the question says the item will be moved");
        check(consoleContains("moved \"") && consoleContains("snap offset set"), "console reports the move and snap offset");
        check(item->position >= 0.0, "item starts at or after the project start");
        check(item->snapOffset > 0.0, "snap offset set (" + std::to_string(item->snapOffset) + " s)");
        // The snap offset (first downbeat) must sit exactly on some bar line at the new tempo.
        const double downbeatProj = item->position + item->snapOffset;
        const double barLen = 60.0 / h.project.bpm * h.project.beatsPerBar;
        const double offGrid = std::fabs(downbeatProj / barLen - std::round(downbeatProj / barLen)) * barLen;
        check(offGrid < 0.005, "first downbeat on a bar line (" + std::to_string(downbeatProj) + " s, " +
                                   std::to_string(offGrid * 1000.0) + " ms off)");
        check(std::fabs(downbeatProj - h.barTime(o.startBar < 1 ? 1 : 2)) < 0.005 || consoleContains("does not fit before bar"),
              "first downbeat on the target bar, or the console explains a later bar");
        check(std::fabs(follows->length - followsLength) > 1e-6, "the bystander on the Beats base was rescaled");
        check(h.undoBlocks.size() == 1, "one undo block");
    } else {
        check(consoleContains("project tempo left at"), "declined: tempo left alone");
        check(std::fabs(h.project.bpm - o.bpm) < 1e-9, "project tempo unchanged");
        check(std::fabs(item->position - o.position) < 1e-9 && item->snapOffset == 0.0, "declined: item not moved, no snap offset");
        check(h.undoBlocks.empty(), "no undo block");
    }
    check(!consoleContains("error:") && !consoleContains("failed on"), "no errors in console");
    unloadExtension();
    return g_failures ? 1 : 0;
}

int runSeparate(int argc, char** argv) {
    if (argc < 3) return 2;
    fs::path media, resource;
    prepareWorkDir(media, resource);
    for (size_t i = 0; i < srt::reaper::kDemucsModelCount; ++i)
        if (!installModel(srt::reaper::kDemucsModels[i], resource)) return 77;
    if (!loadExtension()) return 1;
    auto& h = fake::host();
    MediaTrack* tr = h.addTrack("Mix");
    MediaItem* item = h.addItem(tr, "mix", argv[2], 1.0);
    item->selected = true;

    runAndWait("SRT_SEPARATE_STEMS");
    dumpProject();

    check(h.project.tracks.size() == 6, "mix + folder + 4 stem tracks (" + std::to_string(h.project.tracks.size()) + ")");
    if (h.project.tracks.size() == 6) {
        check(h.project.tracks[1]->folderDepth == 1, "folder track opens a folder");
        check(h.project.tracks[5]->folderDepth < 0, "last stem closes the folder");
        const char* stems[] = {"drums", "bass", "other", "vocals"};
        for (int s = 0; s < 4; ++s) {
            MediaTrack* st = h.project.tracks[static_cast<size_t>(2 + s)].get();
            check(st->name.find(stems[s]) != std::string::npos, "track " + std::to_string(s + 2) + " named for " + stems[s]);
            check(st->items.size() == 1, "one item on " + st->name);
            if (st->items.size() == 1) {
                MediaItem* it = st->items[0].get();
                check(std::fabs(it->position - item->position) < 1e-6, "stem item at the mix position");
                const char* file = it->activeTake() && it->activeTake()->source ? it->activeTake()->source->GetFileName() : nullptr;
                check(file && fs::exists(file) && fs::file_size(file) > 1000, std::string("stem file exists: ") + (file ? file : "none"));
            }
        }
    }
    check(h.undoBlocks.size() == 1, "one undo block");
    check(!consoleContains("error:") && !consoleContains("FAILED"), "no errors in console");
    unloadExtension();
    return g_failures ? 1 : 0;
}

int runAbout() {
    fs::path media, resource;
    prepareWorkDir(media, resource);
    if (!loadExtension()) return 1;
    runAndWait("SRT_ABOUT", 120.0);
    check(consoleContains("Shaunobi's Reaper Tools"), "banner printed");
    check(consoleContains("background check OK"), "background ONNX check passed");
    check(!consoleContains("FAILED"), "no failures reported");
    unloadExtension();
    return g_failures ? 1 : 0;
}

// Interactive: a project with one selected item, commands typed on stdin.
int runShell(int argc, char** argv) {
    if (argc < 3) return 2;
    fs::path media, resource;
    prepareWorkDir(media, resource);
    installModel(srt::reaper::kBeatThisModel, resource);
    for (size_t i = 0; i < srt::reaper::kDemucsModelCount; ++i) installModel(srt::reaper::kDemucsModels[i], resource);
    if (!loadExtension()) return 1;
    auto& h = fake::host();
    MediaTrack* tr = h.addTrack("Track 1");
    MediaItem* item = h.addItem(tr, fs::path(argv[2]).stem().string(), argv[2], 3.0);
    item->selected = true;
    std::printf("commands: SRT_ABOUT SRT_DETECT_TEMPO SRT_FIT_DRUMS_TO_GRID SRT_CONFORM_DRUM_BARS SRT_SEPARATE_STEMS dump bpm <x> startbar <n> answer <6|7> quit\n");
    std::string line;
    while (std::printf("> "), std::fflush(stdout), std::getline(std::cin, line)) {
        if (line == "quit" || line == "q") break;
        if (line == "dump") { dumpProject(); continue; }
        if (line.rfind("bpm ", 0) == 0) { h.project.bpm = std::atof(line.c_str() + 4); continue; }
        if (line.rfind("startbar ", 0) == 0) { h.project.startBar = std::atoi(line.c_str() + 9); continue; }
        if (line.rfind("answer ", 0) == 0) { h.messageBoxAnswer = std::atoi(line.c_str() + 7); continue; }
        if (line.empty()) continue;
        if (!h.commandId(line)) { std::printf("unknown command %s\n", line.c_str()); continue; }
        runAndWait(line);
    }
    unloadExtension();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::error_code ec;
    g_exeDir = fs::canonical(fs::absolute(argv[0]), ec).parent_path();
    if (ec) g_exeDir = fs::absolute(argv[0]).parent_path();
    if (argc >= 3 && std::strcmp(argv[1], "--plugin") == 0) {
        if (!loadPluginLibrary(argv[2])) return 1;
        argv[2] = argv[0];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: srt_fakehost [--plugin <reaper_srt library>] about | fit <wav> [opts] | conform <wav> [opts] | tempo <wav> [opts] | separate <wav> | shell <wav>\n");
        return 2;
    }
    const std::string mode = argv[1];
    int rc;
    if (mode == "about") rc = runAbout();
    else if (mode == "fit") rc = runDrums(argc, argv, false);
    else if (mode == "conform") rc = runDrums(argc, argv, true);
    else if (mode == "tempo") rc = runTempo(argc, argv);
    else if (mode == "separate") rc = runSeparate(argc, argv);
    else if (mode == "shell") rc = runShell(argc, argv);
    else rc = 2;
    if (rc == 2) std::fprintf(stderr, "bad arguments; see usage\n");
    else if (rc == 77) std::printf("RESULT SKIP (model not downloaded; run the unit tests first)\n");
    else std::printf("RESULT %s (%d failed checks)\n", rc == 0 ? "PASS" : "FAIL", g_failures);
    return rc;
}
