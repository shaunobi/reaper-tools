#pragma once
// Plain audio container used throughout srt_core. Planar (one vector per
// channel), 32-bit float, with the sample rate carried alongside.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace srt {

struct AudioBuffer {
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels;

    AudioBuffer() = default;
    AudioBuffer(size_t numChannels, size_t numFrames, double rate)
        : sampleRate(rate), channels(numChannels, std::vector<float>(numFrames, 0.0f)) {}

    size_t numChannels() const { return channels.size(); }
    size_t numFrames() const { return channels.empty() ? 0 : channels[0].size(); }
    double durationSeconds() const {
        return sampleRate > 0.0 ? static_cast<double>(numFrames()) / sampleRate : 0.0;
    }

    // Average of all channels into a single channel.
    std::vector<float> mixToMono() const {
        std::vector<float> out(numFrames(), 0.0f);
        if (channels.empty()) return out;
        const float scale = 1.0f / static_cast<float>(channels.size());
        for (const auto& ch : channels)
            for (size_t i = 0; i < out.size(); ++i) out[i] += ch[i] * scale;
        return out;
    }
};

} // namespace srt
