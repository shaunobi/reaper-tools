#pragma once
// A stand-in for the parts of REAPER the extension talks to: an in-memory
// project (tracks, items, takes, WAV-backed sources, stretch markers,
// extension state), a constant tempo map, and the API functions resolved
// through GetFunc. Semantics follow the REAPER API documentation; where the
// documentation is silent the behaviour is our best understanding and is
// noted in fake_reaper.cpp.

#include "reaper_api.h"
#include "srt/audio.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fake {

struct StretchMarker {
    double pos; // item time, seconds
    double srcpos; // source time, seconds
};

class WavSource;

} // namespace fake

// REAPER's opaque types, given bodies here.
class MediaItem_Take {
public:
    MediaItem* item = nullptr;
    std::string name;
    PCM_source* source = nullptr;
    double startOffset = 0.0;
    double playrate = 1.0;
    std::vector<fake::StretchMarker> markers;
};

class MediaItem {
public:
    MediaTrack* track = nullptr;
    double position = 0.0;
    double length = 0.0;
    bool selected = false;
    double snapOffset = 0.0; // D_SNAPOFFSET: seconds from the item start that snap to the grid
    int timebase = -1; // C_BEATATTACHMODE: -1 track/project default, 0 time, 1 beats, 2 beats (position only)
    std::vector<std::unique_ptr<MediaItem_Take>> takes;
    std::map<std::string, std::string> ext;
    MediaItem_Take* activeTake() const { return takes.empty() ? nullptr : takes.front().get(); }
};

class MediaTrack {
public:
    std::string name;
    int folderDepth = 0;
    int timebase = -1; // C_BEATATTACHMODE on the track
    std::vector<std::unique_ptr<MediaItem>> items;
};

class ReaProject {
public:
    std::vector<std::unique_ptr<MediaTrack>> tracks;
    double bpm = 120.0;
    int beatsPerBar = 4;
    // Displayed bar number of measure index 0. REAPER's "project start measure"
    // setting; 1 means the project starts at bar 1, -2 means bars -2, -1, 0 precede bar 1.
    int startBar = 1;
    int itemTimebase = 1; // project default timebase ("itemtimelock"): 0 time, 1 beats, 2 beats (position only)
    int tempoMarkers = 1; // what CountTempoTimeSigMarkers reports
    std::string mediaDir;
    std::string resourcePath;
};

namespace fake {

struct Host {
    ReaProject project;
    std::vector<std::string> console; // every console line, in order
    int messageBoxAnswer = 6; // IDYES
    std::vector<std::string> messageBoxes; // every question asked, in order
    std::vector<std::string> undoBlocks;
    std::map<std::string, int> commandIds;
    bool (*hookCommand)(int, int) = nullptr;
    std::vector<void (*)()> timers;
    bool echoConsole = true;

    reaper_plugin_info_t pluginInfo();
    int commandId(const std::string& idString) const;
    bool runCommand(const std::string& idString);
    void tick(); // one timer round

    // Project helpers.
    MediaTrack* addTrack(const std::string& name);
    MediaItem* addItem(MediaTrack* track, const std::string& takeName, const std::string& wavPath, double position);
    // A line from the extension's log (timestamped). Fed by the headless log
    // window when the extension is linked in, or by tailing the log file
    // when a separately built extension is loaded with --plugin.
    void logLine(const std::string& stamped);
    double barTime(int displayedBar) const; // project time of a displayed bar's start
    std::vector<std::string> dump() const; // readable project state
};

Host& host();

} // namespace fake
