#include "srt/beat_detect.h"

#include "srt/beat_dbn.h"
#include "srt/onset.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace srt {

const char* toString(Confidence c) {
    switch (c) {
    case Confidence::High: return "high";
    case Confidence::Medium: return "medium";
    case Confidence::Low: return "low";
    }
    return "unknown";
}

namespace {

std::string fmt(double v, int decimals = 1) {
    std::ostringstream s;
    s.precision(decimals);
    s << std::fixed << v;
    return s.str();
}

// Level in dB around a beat: RMS over -10..+80 ms, which covers the hit even
// when the 20 ms model grid places the beat slightly early or late.
double beatLevelDb(const std::vector<float>& mono, double sampleRate, double time) {
    const long long begin = std::max(0LL, static_cast<long long>(std::llround((time - 0.01) * sampleRate)));
    const long long end = std::min(static_cast<long long>(mono.size()), static_cast<long long>(std::llround((time + 0.08) * sampleRate)));
    if (end <= begin) return -120.0;
    double acc = 0.0;
    for (long long i = begin; i < end; ++i) acc += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i)];
    const double rms = std::sqrt(acc / static_cast<double>(end - begin));
    return rms > 1e-9 ? 20.0 * std::log10(rms) : -120.0;
}

} // namespace

std::pair<int, int> gateQuietEnds(const std::vector<float>& mono, double sampleRate, BeatEvents& events, double gateDb) {
    constexpr size_t kMinBeats = 4;
    if (gateDb <= 0.0 || events.beats.size() <= kMinBeats || sampleRate <= 0.0) return {0, 0};
    std::vector<double> level;
    level.reserve(events.beats.size());
    for (double t : events.beats) level.push_back(beatLevelDb(mono, sampleRate, t));
    std::vector<double> sorted = level;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 2), sorted.end());
    const double floor = sorted[sorted.size() / 2] - gateDb;

    size_t first = 0;
    while (first < level.size() && level[first] < floor) ++first;
    size_t last = level.size(); // one past the last kept beat
    while (last > first && level[last - 1] < floor) --last;
    if (last - first < kMinBeats) return {0, 0};
    const int lead = static_cast<int>(first);
    const int trail = static_cast<int>(level.size() - last);
    if (lead == 0 && trail == 0) return {0, 0};

    const double firstTime = events.beats[first] - 1e-6;
    const double lastTime = events.beats[last - 1] + 1e-6;
    events.beats.erase(events.beats.begin() + static_cast<long>(last), events.beats.end());
    events.beats.erase(events.beats.begin(), events.beats.begin() + static_cast<long>(first));
    events.downbeats.erase(std::remove_if(events.downbeats.begin(), events.downbeats.end(),
                                          [&](double t) { return t < firstTime || t > lastTime; }),
                           events.downbeats.end());
    return {lead, trail};
}

BeatDetectResult detectBeats(const BeatTracker& tracker, const AudioBuffer& audio, const BeatDetectOptions& options,
                             Progress* progress) {
    BeatDetectResult r;
    r.provider = tracker.provider();
    BeatActivations act = tracker.activations(audio, progress);

    if (options.useDbn) {
        DbnConfig cfg = options.tempoHintBpm > 0.0 ? dbnConfigForTempo(options.tempoHintBpm, options.tempoTolerance)
                                                    : DbnConfig{};
        r.modelEvents = decodeDbn(act, cfg);
    } else {
        r.modelEvents = pickPeaksMinimal(act);
    }

    std::vector<float> mono;
    if (options.quietGateDb > 0.0 || options.refineOnsets) mono = audio.mixToMono();

    if (options.quietGateDb > 0.0) {
        const auto dropped = gateQuietEnds(mono, audio.sampleRate, r.modelEvents, options.quietGateDb);
        r.gatedLeading = dropped.first;
        r.gatedTrailing = dropped.second;
        if (r.gatedLeading > 0)
            r.notes.push_back("ignored " + std::to_string(r.gatedLeading) + " beat(s) in the lead-in, more than " +
                              fmt(options.quietGateDb, 0) + " dB below the rest (count-in or bleed)");
        if (r.gatedTrailing > 0)
            r.notes.push_back("ignored " + std::to_string(r.gatedTrailing) + " beat(s) at the end, more than " +
                              fmt(options.quietGateDb, 0) + " dB below the rest");
    }
    r.tempoBpm = estimateTempoBpm(r.modelEvents.beats);

    r.events = r.modelEvents;
    if (options.refineOnsets && !r.modelEvents.beats.empty()) {
        if (progress) progress->set(0.96, "refining onsets");
        r.events.beats = refineOnsets(mono, audio.sampleRate, r.modelEvents.beats);
        r.events.downbeats = refineOnsets(mono, audio.sampleRate, r.modelEvents.downbeats);
    }

    // Confidence: three checks, each failure costs one level.
    int failures = 0;
    if (r.modelEvents.beats.size() < 4 || r.modelEvents.downbeats.size() < 2) {
        r.notes.push_back("too few beats or downbeats found");
        failures += 2;
    }
    if (options.tempoHintBpm > 0.0 && r.tempoBpm > 0.0) {
        const double dev = std::fabs(r.tempoBpm - options.tempoHintBpm) / options.tempoHintBpm;
        if (dev > 0.05) {
            r.notes.push_back("detected tempo " + fmt(r.tempoBpm) + " BPM differs from the project tempo " +
                              fmt(options.tempoHintBpm) + " BPM by " + fmt(dev * 100.0) + "%");
            ++failures;
        }
        if (dev > options.tempoTolerance) ++failures;
    }
    if (r.modelEvents.downbeats.size() >= 3) {
        std::vector<double> bars;
        for (size_t i = 1; i < r.modelEvents.downbeats.size(); ++i)
            bars.push_back(r.modelEvents.downbeats[i] - r.modelEvents.downbeats[i - 1]);
        double mean = 0.0;
        for (double b : bars) mean += b;
        mean /= static_cast<double>(bars.size());
        double var = 0.0;
        for (double b : bars) var += (b - mean) * (b - mean);
        const double sd = std::sqrt(var / static_cast<double>(bars.size()));
        if (mean > 0.0 && sd / mean > 0.03) {
            r.notes.push_back("bar lengths vary by " + fmt(100.0 * sd / mean) + "% (irregular downbeats)");
            ++failures;
        }
    }
    if (!r.modelEvents.downbeats.empty()) {
        // Mean downbeat probability at the chosen downbeat frames.
        double sum = 0.0;
        for (double t : r.modelEvents.downbeats) {
            const size_t f = std::min(act.numFrames() - 1, static_cast<size_t>(std::lround(t * act.fps)));
            sum += 1.0 / (1.0 + std::exp(-act.downbeat[f]));
        }
        const double meanProb = sum / static_cast<double>(r.modelEvents.downbeats.size());
        if (meanProb < 0.5) {
            r.notes.push_back("model is unsure about the downbeats (mean probability " + fmt(meanProb, 2) + ")");
            ++failures;
        }
    }
    r.confidence = failures == 0 ? Confidence::High : failures == 1 ? Confidence::Medium : Confidence::Low;
    if (progress) progress->set(1.0, "done");
    return r;
}

} // namespace srt
