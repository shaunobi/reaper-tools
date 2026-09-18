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

} // namespace

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
    r.tempoBpm = estimateTempoBpm(r.modelEvents.beats);

    r.events = r.modelEvents;
    if (options.refineOnsets && !r.modelEvents.beats.empty()) {
        if (progress) progress->set(0.96, "refining onsets");
        std::vector<float> mono = audio.mixToMono();
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
