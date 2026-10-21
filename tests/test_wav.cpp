#include <fstream>
#include "srt/wav.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>

namespace {
srt::AudioBuffer makeTestSignal() {
    srt::AudioBuffer a(2, 1000, 48000.0);
    for (size_t i = 0; i < a.numFrames(); ++i) {
        a.channels[0][i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
        a.channels[1][i] = -0.25f * std::cos(0.02f * static_cast<float>(i));
    }
    return a;
}
std::filesystem::path tempFile(const char* name) { return std::filesystem::temp_directory_path() / name; }
} // namespace

TEST_CASE("WAV float32 round trip is exact") {
    auto a = makeTestSignal();
    auto p = tempFile("srt_test_f32.wav");
    srt::writeWav(p, a, srt::WavFormat::Float32);
    auto b = srt::readWav(p);
    REQUIRE(b.numChannels() == 2);
    REQUIRE(b.numFrames() == 1000);
    REQUIRE(b.sampleRate == 48000.0);
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < 1000; ++i) REQUIRE(b.channels[c][i] == a.channels[c][i]);
    std::filesystem::remove(p);
}

TEST_CASE("WAV PCM16 and PCM24 round trips are within quantisation") {
    auto a = makeTestSignal();
    for (auto [fmt, tol] : {std::pair{srt::WavFormat::Pcm16, 1.0f / 32000.0f},
                            std::pair{srt::WavFormat::Pcm24, 1.0f / 8000000.0f}}) {
        auto p = tempFile("srt_test_pcm.wav");
        srt::writeWav(p, a, fmt);
        auto b = srt::readWav(p);
        REQUIRE(b.numFrames() == a.numFrames());
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < 1000; ++i)
                REQUIRE_THAT(b.channels[c][i], Catch::Matchers::WithinAbs(a.channels[c][i], tol));
        std::filesystem::remove(p);
    }
}

TEST_CASE("readWav rejects a non-WAV file") {
    auto p = tempFile("srt_test_bad.wav");
    {
        std::ofstream f(p, std::ios::binary);
        f << "this is not audio";
    }
    REQUIRE_THROWS(srt::readWav(p));
    std::filesystem::remove(p);
}
