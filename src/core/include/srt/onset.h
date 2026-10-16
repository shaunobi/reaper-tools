#pragma once
// Sample-accurate onset refinement. The beat tracker places events on a 20 ms
// grid; this searches a small window around each event for the strongest
// spectral-flux rise, which on percussive material is the actual hit.

#include <vector>

namespace srt {

struct OnsetRefineConfig {
    double windowSeconds = 0.06; // search +/- this much around the estimate
    double hopSeconds = 0.002; // time resolution of the search
    int fftSize = 512; // analysis frame length in samples (at the audio's rate)
};

// Returns the refined time in seconds, or `time` unchanged if no onset is found.
double refineOnset(const std::vector<float>& mono, double sampleRate, double time,
                   const OnsetRefineConfig& config = OnsetRefineConfig{});

std::vector<double> refineOnsets(const std::vector<float>& mono, double sampleRate, const std::vector<double>& times,
                                 const OnsetRefineConfig& config = OnsetRefineConfig{});

} // namespace srt
