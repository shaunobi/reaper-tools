#include "srt/beat_tracker.h"
#include "srt/wav.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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
std::vector<double> readTimes(const std::filesystem::path& p) {
    std::ifstream f(p);
    REQUIRE(f.good());
    std::vector<double> out;
    double v;
    while (f >> v) out.push_back(v);
    return out;
}
std::filesystem::path modelPath() {
#ifdef SRT_BEAT_THIS_MODEL
    return SRT_BEAT_THIS_MODEL;
#else
    return {};
#endif
}
} // namespace

TEST_CASE("beat tracker reproduces the reference logits and events") {
    const std::filesystem::path model = modelPath();
    if (model.empty() || !std::filesystem::exists(model)) SKIP("Beat This model not available");
    const std::filesystem::path dir = std::filesystem::path(SRT_TEST_DATA_DIR) / "beat";

    srt::BeatTrackerConfig cfg;
    cfg.modelPath = model;
    cfg.providers = {srt::Provider::Cpu};
    srt::BeatTracker tracker(cfg);

    srt::AudioBuffer clip = srt::readWav(dir / "clip.wav");
    srt::BeatActivations act = tracker.activations(clip);

    std::vector<float> refBeat = readF32(dir / "beat.f32");
    std::vector<float> refDown = readF32(dir / "downbeat.f32");
    REQUIRE(act.beat.size() == refBeat.size());
    REQUIRE(act.downbeat.size() == refDown.size());

    float maxDiff = 0.0f;
    for (size_t i = 0; i < refBeat.size(); ++i) {
        maxDiff = std::max(maxDiff, std::fabs(act.beat[i] - refBeat[i]));
        maxDiff = std::max(maxDiff, std::fabs(act.downbeat[i] - refDown[i]));
    }
    INFO("max logit diff " << maxDiff);
    REQUIRE(maxDiff < 5e-3f);

    srt::BeatEvents ev = srt::pickPeaksMinimal(act);
    std::vector<double> refBeats = readTimes(dir / "beats.txt");
    std::vector<double> refDownbeats = readTimes(dir / "downbeats.txt");
    REQUIRE(ev.beats.size() == refBeats.size());
    REQUIRE(ev.downbeats.size() == refDownbeats.size());
    for (size_t i = 0; i < refBeats.size(); ++i)
        REQUIRE_THAT(ev.beats[i], Catch::Matchers::WithinAbs(refBeats[i], 1e-6));
    for (size_t i = 0; i < refDownbeats.size(); ++i)
        REQUIRE_THAT(ev.downbeats[i], Catch::Matchers::WithinAbs(refDownbeats[i], 1e-6));

    // The synthetic clip is at 121.3 BPM with the first downbeat at 0.35 s.
    REQUIRE_THAT(srt::estimateTempoBpm(ev.beats), Catch::Matchers::WithinAbs(121.3, 0.5));
    REQUIRE_THAT(ev.downbeats.front(), Catch::Matchers::WithinAbs(0.35, 0.03));
}

TEST_CASE("beat tracker chunking covers long inputs without gaps") {
    const std::filesystem::path model = modelPath();
    if (model.empty() || !std::filesystem::exists(model)) SKIP("Beat This model not available");
    srt::BeatTrackerConfig cfg;
    cfg.modelPath = model;
    cfg.providers = {srt::Provider::Cpu};
    cfg.chunkFrames = 200; // force several chunks on the 10 s clip
    cfg.borderFrames = 6;
    srt::BeatTracker tracker(cfg);
    srt::AudioBuffer clip = srt::readWav(std::filesystem::path(SRT_TEST_DATA_DIR) / "beat" / "clip.wav");
    srt::BeatActivations act = tracker.activations(clip);
    for (float v : act.beat) REQUIRE(v > -999.0f); // every frame written
    srt::BeatEvents ev = srt::pickPeaksMinimal(act);
    REQUIRE(ev.beats.size() >= 18);
    REQUIRE(ev.downbeats.size() >= 4);
}

TEST_CASE("pickPeaksMinimal merges adjacent peaks and snaps downbeats") {
    srt::BeatActivations act;
    act.beat.assign(100, -5.0f);
    act.downbeat.assign(100, -5.0f);
    act.beat[10] = 2.0f;
    act.beat[11] = 2.0f; // adjacent equal peaks -> averaged to 10.5
    act.beat[50] = 1.0f;
    act.downbeat[52] = 0.5f; // snaps to beat at frame 50
    srt::BeatEvents ev = srt::pickPeaksMinimal(act);
    REQUIRE(ev.beats.size() == 2);
    REQUIRE_THAT(ev.beats[0], Catch::Matchers::WithinAbs(10.5 / 50.0, 1e-9));
    REQUIRE_THAT(ev.beats[1], Catch::Matchers::WithinAbs(1.0, 1e-9));
    REQUIRE(ev.downbeats.size() == 1);
    REQUIRE_THAT(ev.downbeats[0], Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("estimateTempoBpm fits through grid-quantised beats and survives a missed beat") {
    REQUIRE(srt::estimateTempoBpm({}) == 0.0);
    REQUIRE_THAT(srt::estimateTempoBpm({0.0, 0.5, 1.0, 1.5, 2.0}), Catch::Matchers::WithinAbs(120.0, 1e-9));
    // 121.3 BPM beats rounded to a 20 ms grid: the fit recovers the true tempo.
    std::vector<double> beats;
    for (int k = 0; k < 40; ++k) beats.push_back(std::round((0.35 + k * 60.0 / 121.3) * 50.0) / 50.0);
    REQUIRE_THAT(srt::estimateTempoBpm(beats), Catch::Matchers::WithinAbs(121.3, 0.3));
    // Drop one beat in the middle: still the same tempo.
    beats.erase(beats.begin() + 17);
    REQUIRE_THAT(srt::estimateTempoBpm(beats), Catch::Matchers::WithinAbs(121.3, 0.3));
}
