#pragma once
// Reads raw source audio out of a take, in source time, ignoring item
// volume, fades, take effects, playrate and stretch markers.

#include "srt/audio.h"

#include "reaper_api.h"

#include <string>

namespace srt::reaper {

struct SourceInfo {
    bool isAudio = false;
    std::string type;
    double sampleRate = 0.0;
    int channels = 0;
    double lengthSeconds = 0.0;
};

SourceInfo describeSource(MediaItem_Take* take);

// Reads [srcStart, srcStart + length) seconds of the take's source at its
// native rate. Throws std::runtime_error on failure.
srt::AudioBuffer readSourceAudio(MediaItem_Take* take, double srcStart, double length);

} // namespace srt::reaper
