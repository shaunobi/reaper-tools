#include "srt/resample.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

TEST_CASE("resample keeps a tone's frequency and amplitude") {
    const double inRate = 48000.0, outRate = 22050.0, freq = 440.0;
    const size_t n = 48000;
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i) in[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * freq * i / inRate);
    std::vector<float> out = srt::resample(in, inRate, outRate);
    REQUIRE(out.size() == 22050);
    // Compare against the ideal tone in the middle of the buffer (edges carry filter transients).
    double maxErr = 0.0;
    for (size_t i = 2000; i < 20000; ++i) {
        double ideal = 0.5 * std::sin(2.0 * 3.14159265358979 * freq * i / outRate);
        maxErr = std::max(maxErr, std::fabs(out[i] - ideal));
    }
    REQUIRE(maxErr < 1e-3);
}

TEST_CASE("resample is the identity at equal rates") {
    std::vector<float> in{0.1f, 0.2f, 0.3f};
    REQUIRE(srt::resample(in, 44100.0, 44100.0) == in);
}
