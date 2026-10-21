#include "srt/stft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <vector>

TEST_CASE("Stft forward then inverse reconstructs the signal") {
    const size_t n = 44100;
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i) in[i] = 0.4f * std::sin(0.01f * i) + 0.2f * std::sin(0.31f * i + 1.0f);
    srt::Stft stft(2048, 441);
    std::vector<std::complex<float>> spec;
    stft.forward(in.data(), n, spec);
    REQUIRE(spec.size() == stft.numFrames(n) * stft.numBins());
    REQUIRE(stft.numFrames(n) == 101);
    std::vector<float> out;
    stft.inverse(spec.data(), stft.numFrames(n), n, out);
    REQUIRE(out.size() == n);
    for (size_t i = 0; i < n; ++i) REQUIRE_THAT(out[i], Catch::Matchers::WithinAbs(in[i], 1e-4f));
}

TEST_CASE("Stft frame count and window match torch conventions") {
    srt::Stft stft(2048, 441);
    REQUIRE(stft.numFrames(352800) == 801); // the model's 8 s chunk
    REQUIRE(stft.window()[0] == 0.0f);
    REQUIRE_THAT(stft.window()[1024], Catch::Matchers::WithinAbs(1.0f, 1e-6f)); // periodic Hann peaks at N/2
    REQUIRE(stft.window()[2047] > 0.0f); // periodic: last sample is not zero
}
