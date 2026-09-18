#include "srt/separator.h"
#include "srt/wav.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstring>
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
double corr(const std::vector<float>& a, const std::vector<float>& b) {
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
    ma /= a.size(); mb /= b.size();
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return sab / std::sqrt(saa * sbb);
}
} // namespace

TEST_CASE("separateWith reproduces the input through an identity model with overlap-add") {
    const double rate = 44100.0;
    srt::AudioBuffer stereo(2, 30000, rate); // shorter than a chunk, so it is zero-padded
    for (size_t i = 0; i < stereo.numFrames(); ++i) {
        stereo.channels[0][i] = 0.5f * std::sin(0.02f * i);
        stereo.channels[1][i] = 0.3f * std::sin(0.05f * i + 0.3f);
    }
    // Identity for stem 0, half gain for stem 1.
    auto model = [](const srt::ChunkSpectrum& in, srt::ChunkSpectrum& out) {
        out.real.resize(in.real.size() * 2);
        out.imag.resize(in.imag.size() * 2);
        std::copy(in.real.begin(), in.real.end(), out.real.begin());
        std::copy(in.imag.begin(), in.imag.end(), out.imag.begin());
        for (size_t i = 0; i < in.real.size(); ++i) {
            out.real[in.real.size() + i] = 0.5f * in.real[i];
            out.imag[in.imag.size() + i] = 0.5f * in.imag[i];
        }
    };
    const size_t chunk = 8820; // 20 frames of 441
    std::vector<srt::AudioBuffer> stems = srt::StemSeparator::separateWith(stereo, model, 2, chunk, 2048, 441, 0.5);
    REQUIRE(stems.size() == 2);
    REQUIRE(stems[0].numFrames() == stereo.numFrames());
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = 0; i < stereo.numFrames(); ++i) {
            REQUIRE_THAT(stems[0].channels[ch][i], Catch::Matchers::WithinAbs(stereo.channels[ch][i], 2e-4f));
            REQUIRE_THAT(stems[1].channels[ch][i], Catch::Matchers::WithinAbs(0.5f * stereo.channels[ch][i], 2e-4f));
        }
}

TEST_CASE("BS-RoFormer separation matches the Python reference on the synthetic mix") {
#ifdef SRT_SEPARATION_MODEL
    const std::filesystem::path model = SRT_SEPARATION_MODEL;
#else
    const std::filesystem::path model;
#endif
    if (model.empty() || !std::filesystem::exists(model)) SKIP("separation model not available");
    const std::filesystem::path dir = std::filesystem::path(SRT_TEST_DATA_DIR) / "sep";
    srt::AudioBuffer mix = srt::readWav(dir / "mix.wav");
    REQUIRE(mix.numChannels() == 2);
    REQUIRE(mix.sampleRate == 44100.0);

    srt::SeparatorConfig cfg;
    cfg.modelPath = model;
    cfg.providers = {srt::Provider::Cpu};
    srt::StemSeparator sep(cfg);
    REQUIRE(sep.numStems() == 4);
    REQUIRE(sep.chunkSamples() == 352800);
    std::vector<srt::AudioBuffer> stems = sep.separate(mix);
    REQUIRE(stems.size() == 4);
    REQUIRE(stems[0].numFrames() == mix.numFrames());

    const size_t a = 66150, b = 88200; // [1.5 s, 2.0 s)
    const char* names[] = {"drums", "bass", "other", "vocals"};
    for (size_t s = 0; s < 4; ++s) {
        std::vector<float> ref = readF32(dir / (std::string(names[s]) + ".f32")); // interleaved stereo
        REQUIRE(ref.size() == (b - a) * 2);
        float maxDiff = 0.0f, maxRef = 0.0f;
        for (size_t i = a; i < b; ++i)
            for (size_t ch = 0; ch < 2; ++ch) {
                maxDiff = std::max(maxDiff, std::fabs(stems[s].channels[ch][i] - ref[(i - a) * 2 + ch]));
                maxRef = std::max(maxRef, std::fabs(ref[(i - a) * 2 + ch]));
            }
        INFO(names[s] << ": max diff " << maxDiff << ", max ref " << maxRef);
        REQUIRE(maxDiff < 2e-3f);
    }

    // Sanity: the drums stem resembles the drum part more than the mix does.
    std::vector<float> part = readF32(dir / "drums_part.f32");
    std::vector<float> stemSlice, mixSlice;
    for (size_t i = a; i < b; ++i)
        for (size_t ch = 0; ch < 2; ++ch) {
            stemSlice.push_back(stems[0].channels[ch][i]);
            mixSlice.push_back(mix.channels[ch][i]);
        }
    const double cStem = corr(stemSlice, part), cMix = corr(mixSlice, part);
    INFO("corr drums stem " << cStem << ", corr mix " << cMix);
    REQUIRE(cStem > cMix);
}
