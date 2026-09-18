#include "srt/fft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

TEST_CASE("RealFFT forward then inverse reproduces the input") {
    const size_t n = 1024;
    srt::RealFFT fft(n);
    std::vector<float> in(n), out(n);
    for (size_t i = 0; i < n; ++i) in[i] = std::sin(0.1f * i) + 0.3f * std::cos(0.37f * i);
    std::vector<std::complex<float>> bins(fft.numBins());
    fft.forward(in.data(), bins.data());
    fft.inverse(bins.data(), out.data());
    for (size_t i = 0; i < n; ++i) REQUIRE_THAT(out[i], Catch::Matchers::WithinAbs(in[i], 1e-4f));
}

TEST_CASE("RealFFT puts a pure tone in the right bin") {
    const size_t n = 512;
    const size_t k = 37;
    srt::RealFFT fft(n);
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i) in[i] = std::cos(2.0f * 3.14159265f * k * i / n);
    std::vector<std::complex<float>> bins(fft.numBins());
    fft.forward(in.data(), bins.data());
    size_t peak = 0;
    for (size_t b = 1; b < bins.size(); ++b)
        if (std::abs(bins[b]) > std::abs(bins[peak])) peak = b;
    REQUIRE(peak == k);
    REQUIRE_THAT(std::abs(bins[k]), Catch::Matchers::WithinRel(n / 2.0f, 1e-3f));
}
