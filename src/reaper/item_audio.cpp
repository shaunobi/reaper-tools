#include "item_audio.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace srt::reaper {

SourceInfo describeSource(MediaItem_Take* take) {
    SourceInfo info;
    PCM_source* src = take ? GetMediaItemTake_Source(take) : nullptr;
    if (!src) return info;
    char type[64] = {0};
    GetMediaSourceType(src, type, sizeof(type));
    info.type = type;
    bool lengthIsQn = false;
    info.lengthSeconds = GetMediaSourceLength(src, &lengthIsQn);
    info.sampleRate = GetMediaSourceSampleRate(src);
    info.channels = GetMediaSourceNumChannels(src);
    info.isAudio = !lengthIsQn && info.sampleRate >= 1.0 && info.channels > 0 && info.type != "MIDI" &&
                   info.type != "MIDIPOOL" && info.type != "EMPTY" && info.type != "VIDEO";
    return info;
}

srt::AudioBuffer readSourceAudio(MediaItem_Take* take, double srcStart, double length) {
    SourceInfo info = describeSource(take);
    if (!info.isAudio) throw std::runtime_error("take has no audio source");
    PCM_source* src = GetMediaItemTake_Source(take);
    const double rate = info.sampleRate;
    const int nch = info.channels;
    const double clampedStart = std::max(0.0, srcStart);
    const double clampedEnd = std::min(info.lengthSeconds, srcStart + length);
    const long totalFrames = static_cast<long>(std::llround((clampedEnd - clampedStart) * rate));
    if (totalFrames <= 0) throw std::runtime_error("item covers no audio");

    srt::AudioBuffer out(static_cast<size_t>(nch), static_cast<size_t>(totalFrames), rate);
    const int blockFrames = 65536;
    std::vector<ReaSample> block(static_cast<size_t>(blockFrames) * nch);
    long done = 0;
    while (done < totalFrames) {
        const int want = static_cast<int>(std::min<long>(blockFrames, totalFrames - done));
        PCM_source_transfer_t t{};
        t.time_s = clampedStart + static_cast<double>(done) / rate;
        t.samplerate = rate;
        t.nch = nch;
        t.length = want;
        t.samples = block.data();
        t.samples_out = 0;
        src->GetSamples(&t);
        if (t.samples_out <= 0) break;
        for (int f = 0; f < t.samples_out; ++f)
            for (int c = 0; c < nch; ++c)
                out.channels[static_cast<size_t>(c)][static_cast<size_t>(done + f)] =
                    static_cast<float>(block[static_cast<size_t>(f) * nch + c]);
        done += t.samples_out;
    }
    if (done < totalFrames)
        for (auto& ch : out.channels) ch.resize(static_cast<size_t>(done));
    if (done == 0) throw std::runtime_error("source returned no samples");
    return out;
}

} // namespace srt::reaper
