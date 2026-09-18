#include "srt/onset.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>
#include <vector>

TEST_CASE("refineOnset locks onto a noise-burst hit near the estimate") {
    const double rate = 44100.0;
    std::vector<float> mono(static_cast<size_t>(rate * 2.0), 0.0f);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const std::vector<double> hits{0.4, 0.9127, 1.5};
    for (double h : hits) {
        size_t start = static_cast<size_t>(std::lround(h * rate));
        for (size_t i = 0; i < 4000 && start + i < mono.size(); ++i)
            mono[start + i] += 0.6f * noise(rng) * std::exp(-static_cast<float>(i) / 900.0f);
    }
    // Low-level noise floor so the search never sees pure silence.
    for (float& v : mono) v += 0.002f * noise(rng);

    for (double h : hits) {
        double refined = srt::refineOnset(mono, rate, h + 0.035); // estimate 35 ms late
        REQUIRE_THAT(refined, Catch::Matchers::WithinAbs(h, 0.004));
        refined = srt::refineOnset(mono, rate, h - 0.04); // estimate 40 ms early
        REQUIRE_THAT(refined, Catch::Matchers::WithinAbs(h, 0.004));
    }
}

TEST_CASE("refineOnset leaves the estimate alone in silence") {
    std::vector<float> mono(44100, 0.0f);
    REQUIRE(srt::refineOnset(mono, 44100.0, 0.5) == 0.5);
}
