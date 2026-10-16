#pragma once
// Sample-rate conversion (r8brain, 24-bit quality preset). Offline only.

#include "srt/audio.h"

#include <vector>

namespace srt {

// Returns the signal resampled from inRate to outRate. Output length is
// round(in.size() * outRate / inRate). Identity when the rates match.
std::vector<float> resample(const std::vector<float>& in, double inRate, double outRate);

AudioBuffer resample(const AudioBuffer& in, double outRate);

} // namespace srt
