#include "srt/resample.h"

#include "CDSPResampler.h"

#include <cmath>
#include <stdexcept>

namespace srt {

std::vector<float> resample(const std::vector<float>& in, double inRate, double outRate) {
    if (inRate <= 0.0 || outRate <= 0.0) throw std::invalid_argument("resample: rates must be positive");
    if (inRate == outRate || in.empty()) return in;

    const size_t outLen = static_cast<size_t>(std::llround(static_cast<double>(in.size()) * outRate / inRate));
    std::vector<double> src(in.begin(), in.end());
    std::vector<double> dst(outLen);
    const int maxInLen = 4096;
    r8b::CDSPResampler24 resampler(inRate, outRate, maxInLen);
    resampler.oneshot(src.data(), static_cast<int>(src.size()), dst.data(), static_cast<int>(dst.size()));
    return std::vector<float>(dst.begin(), dst.end());
}

AudioBuffer resample(const AudioBuffer& in, double outRate) {
    if (in.sampleRate == outRate) return in;
    AudioBuffer out;
    out.sampleRate = outRate;
    out.channels.reserve(in.numChannels());
    for (const auto& ch : in.channels) out.channels.push_back(resample(ch, in.sampleRate, outRate));
    return out;
}

} // namespace srt
