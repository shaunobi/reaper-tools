#include <cstring>
#include "srt/mel.h"
#include "srt/wav.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
std::vector<float> readF32(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<float> out(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), out.size() * 4);
    return out;
}
} // namespace

TEST_CASE("log-mel front end matches torchaudio on the reference clip") {
    const std::filesystem::path dir = std::filesystem::path(SRT_TEST_DATA_DIR) / "beat";
    srt::AudioBuffer clip = srt::readWav(dir / "clip.wav");
    REQUIRE(clip.sampleRate == 22050.0);
    REQUIRE(clip.numChannels() == 1);

    srt::LogMelSpectrogram mel;
    const size_t frames = mel.numFrames(clip.numFrames());
    std::vector<float> ours = mel.compute(clip.channels[0].data(), clip.numFrames());
    std::vector<float> ref = readF32(dir / "spect.f32");
    REQUIRE(ours.size() == ref.size());
    REQUIRE(frames * 128 == ref.size());

    float maxDiff = 0.0f, maxVal = 0.0f;
    for (size_t i = 0; i < ref.size(); ++i) {
        maxDiff = std::max(maxDiff, std::fabs(ours[i] - ref[i]));
        maxVal = std::max(maxVal, std::fabs(ref[i]));
    }
    INFO("max abs diff " << maxDiff << " (max value " << maxVal << ")");
    REQUIRE(maxDiff < 2e-4f);
}

TEST_CASE("Slaney filterbank has the expected shape and triangle peaks") {
    std::vector<double> fb = srt::LogMelSpectrogram::slaneyFilterbank(1024, 22050, 128, 30.0, 11000.0);
    REQUIRE(fb.size() == 128u * 513u);
    // Every filter is non-negative, peaks at most 1, and bands cover the range without gaps.
    std::vector<double> coverage(513, 0.0);
    for (size_t m = 0; m < 128; ++m) {
        double peak = 0.0;
        for (size_t k = 0; k < 513; ++k) {
            REQUIRE(fb[m * 513 + k] >= 0.0);
            peak = std::max(peak, fb[m * 513 + k]);
            coverage[k] += fb[m * 513 + k];
        }
        REQUIRE(peak <= 1.0 + 1e-12);
        REQUIRE(peak > 0.0);
    }
    // Bins between 60 Hz and 10.9 kHz must be covered by at least one filter.
    for (size_t k = 3; k < 506; ++k) REQUIRE(coverage[k] > 0.0);
}
