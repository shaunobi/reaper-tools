#include "srt/beat_dbn.h"
#include "srt/beat_detect.h"
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
std::vector<double> readTimes(const std::filesystem::path& p) {
    std::ifstream f(p);
    REQUIRE(f.good());
    std::vector<double> out;
    double v;
    while (f >> v) out.push_back(v);
    return out;
}
srt::BeatActivations referenceActivations() {
    const std::filesystem::path dir = std::filesystem::path(SRT_TEST_DATA_DIR) / "beat";
    srt::BeatActivations act;
    act.beat = readF32(dir / "beat.f32");
    act.downbeat = readF32(dir / "downbeat.f32");
    return act;
}
void requireClose(const std::vector<double>& got, const std::vector<double>& want, double tol) {
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) REQUIRE_THAT(got[i], Catch::Matchers::WithinAbs(want[i], tol));
}
} // namespace

TEST_CASE("DBN decoding agrees with peak picking on clean activations") {
    const std::filesystem::path dir = std::filesystem::path(SRT_TEST_DATA_DIR) / "beat";
    srt::BeatActivations act = referenceActivations();
    srt::BeatEvents ev = srt::decodeDbn(act);
    requireClose(ev.beats, readTimes(dir / "beats.txt"), 0.021);
    requireClose(ev.downbeats, readTimes(dir / "downbeats.txt"), 0.021);
}

TEST_CASE("DBN decoding recovers a dropped beat and ignores a spurious one") {
    const std::filesystem::path dir = std::filesystem::path(SRT_TEST_DATA_DIR) / "beat";
    std::vector<double> refBeats = readTimes(dir / "beats.txt");
    srt::BeatActivations act = referenceActivations();
    // Erase the 8th beat (frames around 3.82 s) and add a fake peak between beats 12 and 13.
    const size_t dropped = static_cast<size_t>(std::lround(refBeats[7] * 50.0));
    for (size_t f = dropped - 3; f <= dropped + 3; ++f) act.beat[f] = -6.0f;
    const size_t fake = static_cast<size_t>(std::lround((refBeats[11] + refBeats[12]) / 2.0 * 50.0));
    act.beat[fake] = 3.0f;

    srt::BeatEvents naive = srt::pickPeaksMinimal(act);
    REQUIRE(naive.beats.size() == refBeats.size()); // one lost, one gained: same count, wrong places
    bool naiveHasFake = false;
    for (double b : naive.beats)
        if (std::fabs(b - fake / 50.0) < 1e-6) naiveHasFake = true;
    REQUIRE(naiveHasFake);

    srt::BeatEvents ev = srt::decodeDbn(act);
    REQUIRE(ev.beats.size() == refBeats.size());
    for (size_t i = 0; i < refBeats.size(); ++i) REQUIRE_THAT(ev.beats[i], Catch::Matchers::WithinAbs(refBeats[i], 0.061));
    REQUIRE(ev.downbeats.size() == 5);
}

TEST_CASE("DBN tempo prior constrains the decoded tempo") {
    srt::BeatActivations act = referenceActivations();
    srt::BeatEvents fast = srt::decodeDbn(act, srt::dbnConfigForTempo(121.0, 0.15));
    REQUIRE(fast.beats.size() == 20);
    REQUIRE_THAT(srt::estimateTempoBpm(fast.beats), Catch::Matchers::WithinAbs(121.3, 1.0));
    // Forced to half time, the decoder must land on every other beat.
    srt::BeatEvents half = srt::decodeDbn(act, srt::dbnConfigForTempo(60.5, 0.15));
    REQUIRE(half.beats.size() >= 9);
    REQUIRE(half.beats.size() <= 11);
    REQUIRE_THAT(srt::estimateTempoBpm(half.beats), Catch::Matchers::WithinAbs(60.65, 1.5));
}

TEST_CASE("DBN returns nothing on silence") {
    srt::BeatActivations act;
    act.beat.assign(500, -10.0f);
    act.downbeat.assign(500, -10.0f);
    srt::BeatEvents ev = srt::decodeDbn(act);
    REQUIRE(ev.beats.empty());
    REQUIRE(ev.downbeats.empty());
}

TEST_CASE("detectBeats reports high confidence on the reference clip with the right tempo hint") {
#ifdef SRT_BEAT_THIS_MODEL
    const std::filesystem::path model = SRT_BEAT_THIS_MODEL;
#else
    const std::filesystem::path model;
#endif
    if (model.empty() || !std::filesystem::exists(model)) SKIP("Beat This model not available");
    srt::BeatTrackerConfig cfg;
    cfg.modelPath = model;
    cfg.providers = {srt::Provider::Cpu};
    srt::BeatTracker tracker(cfg);
    srt::AudioBuffer clip = srt::readWav(std::filesystem::path(SRT_TEST_DATA_DIR) / "beat" / "clip.wav");

    srt::BeatDetectOptions opt;
    opt.tempoHintBpm = 120.0;
    srt::BeatDetectResult r = srt::detectBeats(tracker, clip, opt);
    INFO("notes: " << (r.notes.empty() ? "" : r.notes.front()));
    REQUIRE(r.confidence == srt::Confidence::High);
    REQUIRE_THAT(r.tempoBpm, Catch::Matchers::WithinAbs(121.3, 1.0));
    REQUIRE(r.events.downbeats.size() == 5);
    // Refined downbeats land within 3 ms of the true synthetic hits.
    const double bar = 4.0 * 60.0 / 121.3;
    for (size_t i = 0; i < 5; ++i)
        REQUIRE_THAT(r.events.downbeats[i], Catch::Matchers::WithinAbs(0.35 + i * bar, 0.003));

    // A wrong hint far from the truth drops the confidence and explains why.
    opt.tempoHintBpm = 95.0;
    srt::BeatDetectResult wrong = srt::detectBeats(tracker, clip, opt);
    REQUIRE(wrong.confidence != srt::Confidence::High);
    REQUIRE(!wrong.notes.empty());
}
