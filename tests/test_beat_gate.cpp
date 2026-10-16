#include "srt/beat_detect.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

// Mono buffer with a short noise burst at each beat, at the given amplitude.
std::vector<float> clicks(double sampleRate, const std::vector<double>& times, const std::vector<double>& amps,
                          double seconds) {
    std::vector<float> mono(static_cast<size_t>(seconds * sampleRate), 0.0f);
    unsigned seed = 1;
    for (size_t k = 0; k < times.size(); ++k) {
        const size_t start = static_cast<size_t>(times[k] * sampleRate);
        const size_t len = static_cast<size_t>(0.03 * sampleRate);
        for (size_t i = start; i < start + len && i < mono.size(); ++i) {
            seed = seed * 1103515245u + 12345u;
            const double n = (static_cast<double>((seed >> 8) & 0xffff) / 32768.0) - 1.0;
            mono[i] = static_cast<float>(amps[k] * n);
        }
    }
    return mono;
}

} // namespace

TEST_CASE("quiet-ends gate drops a faint count-in and the phantom downbeat in it") {
    const double sr = 22050.0;
    std::vector<double> times, amps;
    for (int i = 0; i < 12; ++i) {
        times.push_back(0.25 + 0.5 * i);
        amps.push_back(i < 3 ? 0.01 : 0.8); // first three beats 38 dB down: count-in bleed
    }
    std::vector<float> mono = clicks(sr, times, amps, 7.0);
    srt::BeatEvents ev;
    ev.beats = times;
    ev.downbeats = {times[1], times[5], times[9]}; // a phantom downbeat inside the count-in

    const auto dropped = srt::gateQuietEnds(mono, sr, ev, 20.0);
    REQUIRE(dropped.first == 3);
    REQUIRE(dropped.second == 0);
    REQUIRE(ev.beats.size() == 9);
    REQUIRE(ev.beats.front() == times[3]);
    REQUIRE(ev.downbeats.size() == 2);
    REQUIRE(ev.downbeats.front() == times[5]);
}

TEST_CASE("quiet-ends gate trims a fade-out at the end and leaves the middle alone") {
    const double sr = 22050.0;
    std::vector<double> times, amps;
    for (int i = 0; i < 12; ++i) {
        times.push_back(0.25 + 0.5 * i);
        amps.push_back(i >= 10 ? 0.005 : (i == 5 ? 0.2 : 0.8)); // one softer beat mid-way, two near-silent at the end
    }
    std::vector<float> mono = clicks(sr, times, amps, 7.0);
    srt::BeatEvents ev;
    ev.beats = times;
    ev.downbeats = {times[0], times[4], times[8], times[11]};

    const auto dropped = srt::gateQuietEnds(mono, sr, ev, 20.0);
    REQUIRE(dropped.first == 0);
    REQUIRE(dropped.second == 2);
    REQUIRE(ev.beats.size() == 10);
    REQUIRE(ev.downbeats.size() == 3);
}

TEST_CASE("quiet-ends gate does nothing when disabled, when levels are even, or when it would leave too few beats") {
    const double sr = 22050.0;
    std::vector<double> times, amps;
    for (int i = 0; i < 6; ++i) {
        times.push_back(0.25 + 0.5 * i);
        amps.push_back(0.5);
    }
    std::vector<float> mono = clicks(sr, times, amps, 4.0);
    srt::BeatEvents ev;
    ev.beats = times;
    ev.downbeats = {times[0], times[4]};
    REQUIRE(srt::gateQuietEnds(mono, sr, ev, 20.0) == std::pair<int, int>{0, 0});
    REQUIRE(ev.beats.size() == 6);

    // Three loud beats after three quiet ones: gating would leave fewer than four, so nothing is dropped.
    amps = {0.01, 0.01, 0.01, 0.8, 0.8, 0.8};
    mono = clicks(sr, times, amps, 4.0);
    REQUIRE(srt::gateQuietEnds(mono, sr, ev, 20.0) == std::pair<int, int>{0, 0});
    REQUIRE(ev.beats.size() == 6);

    // Disabled.
    REQUIRE(srt::gateQuietEnds(mono, sr, ev, 0.0) == std::pair<int, int>{0, 0});
}
