#include "srt/demucs.h"
#include "srt/wav.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
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

TEST_CASE("Demucs host spectrogram and its inverse round-trip a segment") {
    const size_t segment = 343980; // 7.8 s at 44.1 kHz
    srt::Stft stft(4096, 1024);
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 0.3f);
    std::vector<float> x(segment);
    for (float& v : x) v = noise(rng);
    std::vector<float> real, imag, back;
    srt::DemucsSeparator::spec(stft, x.data(), segment, real, imag);
    const size_t frames = srt::DemucsSeparator::specFrames(segment, 1024);
    REQUIRE(frames == 336);
    REQUIRE(real.size() == 2048 * frames);
    srt::DemucsSeparator::ispec(stft, real.data(), imag.data(), segment, back);
    REQUIRE(back.size() == segment);
    // HTDemucs zeroes the two outer frames on each side before the inverse,
    // so the first and last ~nfft samples of a segment are inexact by design
    // (the segment weights go to zero there). The interior must be exact
    // apart from the dropped Nyquist bin.
    float maxDiff = 0.0f, edgeDiff = 0.0f;
    for (size_t i = 0; i < segment; ++i) {
        const float d = std::fabs(back[i] - x[i]);
        if (i >= 4096 && i + 4096 < segment) maxDiff = std::max(maxDiff, d);
        else edgeDiff = std::max(edgeDiff, d);
    }
    INFO("max round-trip error interior " << maxDiff << ", edges " << edgeDiff);
    REQUIRE(maxDiff < 2e-2f); // the Nyquist bin of white noise is the residual
}

TEST_CASE("Demucs segmenting reproduces the input through identity models with overlap-add") {
    // 20 s of audio: three overlapping 7.8 s segments, the last one short and centre-padded.
    const double rate = 44100.0;
    const size_t n = 882000;
    srt::AudioBuffer stereo(2, n, rate);
    for (size_t i = 0; i < n; ++i) {
        stereo.channels[0][i] = 0.5f * std::sin(0.013f * i) + 0.1f * std::sin(0.3f * i);
        stereo.channels[1][i] = 0.4f * std::sin(0.021f * i + 0.7f);
    }
    const size_t segment = 343980, bins = 2048, frames = 336;
    // Stem 0: everything through the spectral branch. Stem 1: everything through the time branch.
    auto model = [&](const srt::DemucsSegmentIn& in, srt::DemucsSegmentOut& out) {
        out.specReal.assign(2 * 2 * bins * frames, 0.0f);
        out.specImag.assign(2 * 2 * bins * frames, 0.0f);
        out.time.assign(2 * 2 * segment, 0.0f);
        std::copy(in.specReal.begin(), in.specReal.end(), out.specReal.begin());
        std::copy(in.specImag.begin(), in.specImag.end(), out.specImag.begin());
        std::copy(in.mix.begin(), in.mix.end(), out.time.begin() + static_cast<long>(2 * segment));
    };
    std::vector<srt::AudioBuffer> stems = srt::DemucsSeparator::separateWith(stereo, model, 2, segment, 4096, 1024, 0.25);
    REQUIRE(stems.size() == 2);
    REQUIRE(stems[0].numFrames() == n);
    // The file's first and last ~nfft samples only get a segment edge (see the
    // round-trip test); everywhere else the blend of segments must be exact.
    float maxSpec = 0.0f, maxTime = 0.0f;
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = 4096; i + 4096 < n; ++i) {
            maxSpec = std::max(maxSpec, std::fabs(stems[0].channels[ch][i] - stereo.channels[ch][i]));
            maxTime = std::max(maxTime, std::fabs(stems[1].channels[ch][i] - stereo.channels[ch][i]));
        }
    INFO("max error spectral branch " << maxSpec << ", time branch " << maxTime);
    REQUIRE(maxSpec < 2e-3f);
    REQUIRE(maxTime < 1e-6f);
}

TEST_CASE("Demucs v4 fine-tuned separation matches the Python reference on the synthetic mix") {
#ifdef SRT_DEMUCS_MODEL_DIR
    const std::filesystem::path dir = SRT_DEMUCS_MODEL_DIR;
#else
    const std::filesystem::path dir;
#endif
    const std::filesystem::path refDir = std::filesystem::path(SRT_TEST_DATA_DIR) / "sep" / "demucs";
    const char* names[] = {"drums", "bass", "other", "vocals"};
    srt::DemucsConfig cfg;
    for (const char* s : names) cfg.modelPaths.push_back(dir / (std::string("htdemucs_ft_") + s + ".onnx"));
    for (const auto& p : cfg.modelPaths)
        if (dir.empty() || !std::filesystem::exists(p)) SKIP("Demucs models not available");
    if (!std::filesystem::exists(refDir / "drums.f32")) SKIP("Demucs reference not available");

    srt::AudioBuffer mix = srt::readWav(std::filesystem::path(SRT_TEST_DATA_DIR) / "sep" / "mix.wav");
    REQUIRE(mix.numChannels() == 2);
    srt::DemucsSeparator sep(cfg); // best provider available: the GPU where there is one
    INFO("provider " << srt::toString(sep.provider()));
    REQUIRE(sep.numStems() == 4);
    REQUIRE(sep.segmentSamples() == 343980);
    std::vector<srt::AudioBuffer> stems = sep.separate(mix);
    REQUIRE(stems.size() == 4);
    REQUIRE(stems[0].numFrames() == mix.numFrames());

    const size_t a = 66150, b = 88200; // [1.5 s, 2.0 s)
    for (size_t s = 0; s < 4; ++s) {
        std::vector<float> ref = readF32(refDir / (std::string(names[s]) + ".f32"));
        REQUIRE(ref.size() == (b - a) * 2);
        float maxDiff = 0.0f, maxRef = 0.0f;
        for (size_t i = a; i < b; ++i)
            for (size_t ch = 0; ch < 2; ++ch) {
                maxDiff = std::max(maxDiff, std::fabs(stems[s].channels[ch][i] - ref[(i - a) * 2 + ch]));
                maxRef = std::max(maxRef, std::fabs(ref[(i - a) * 2 + ch]));
            }
        INFO(names[s] << ": max diff " << maxDiff << ", max ref " << maxRef << " (" << srt::toString(sep.provider()) << ")");
        REQUIRE(maxDiff < 5e-3f);
    }
}
