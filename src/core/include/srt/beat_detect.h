#pragma once
// The complete detection pipeline from the spec (section 7): model, decoding
// with an optional project-tempo prior, onset refinement, and a confidence
// verdict with the reasons behind it.

#include "srt/audio.h"
#include "srt/beat_tracker.h"
#include "srt/jobs.h"

#include <string>
#include <vector>

namespace srt {

struct BeatDetectOptions {
    bool useDbn = true; // tempo-constrained decoding; false = reference peak picking
    double tempoHintBpm = 0.0; // project tempo; 0 = unconstrained (55-215 BPM)
    double tempoTolerance = 0.15; // allowed deviation from the hint
    bool refineOnsets = true;
};

enum class Confidence { High, Medium, Low };
const char* toString(Confidence c);

struct BeatDetectResult {
    BeatEvents events; // final positions in seconds (refined if requested)
    BeatEvents modelEvents; // positions on the model's 20 ms grid
    double tempoBpm = 0.0;
    Confidence confidence = Confidence::Low;
    std::vector<std::string> notes; // why confidence is not high
    Provider provider = Provider::Cpu;
};

BeatDetectResult detectBeats(const BeatTracker& tracker, const AudioBuffer& audio,
                             const BeatDetectOptions& options = BeatDetectOptions{}, Progress* progress = nullptr);

} // namespace srt
