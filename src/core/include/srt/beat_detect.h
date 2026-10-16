#pragma once
// The complete detection pipeline from the spec (section 7): model, decoding
// with an optional project-tempo prior, onset refinement, and a confidence
// verdict with the reasons behind it.

#include "srt/audio.h"
#include "srt/beat_tracker.h"
#include "srt/jobs.h"

#include <string>
#include <utility>
#include <vector>

namespace srt {

struct BeatDetectOptions {
    bool useDbn = true; // tempo-constrained decoding; false = reference peak picking
    double tempoHintBpm = 0.0; // project tempo; 0 = unconstrained (55-215 BPM)
    double tempoTolerance = 0.15; // allowed deviation from the hint
    bool refineOnsets = true;
    // Quiet-ends gate: beats at the start and end of the audio whose level is
    // more than this many dB below the typical beat are dropped, along with
    // any downbeats before/after them. Catches count-in bleed and room noise
    // in the lead-in, which the model otherwise turns into phantom bars.
    // 0 disables.
    double quietGateDb = 20.0;
};

// The quiet-ends gate on its own: measures the level around each beat
// (RMS over -10..+80 ms), takes the median over all beats as the reference,
// and trims leading and trailing beats that sit more than gateDb below it.
// Downbeats outside the surviving beats are removed too. Never trims to fewer
// than four beats. Returns the numbers of beats dropped at the start and end.
std::pair<int, int> gateQuietEnds(const std::vector<float>& mono, double sampleRate, BeatEvents& events,
                                  double gateDb);

enum class Confidence { High, Medium, Low };
const char* toString(Confidence c);

struct BeatDetectResult {
    BeatEvents events; // final positions in seconds (refined if requested)
    BeatEvents modelEvents; // positions on the model's 20 ms grid
    double tempoBpm = 0.0;
    Confidence confidence = Confidence::Low;
    std::vector<std::string> notes; // why confidence is not high, and what the quiet-ends gate dropped
    Provider provider = Provider::Cpu;
    int gatedLeading = 0; // beats dropped by the quiet-ends gate at the start
    int gatedTrailing = 0; // ... and at the end
};

BeatDetectResult detectBeats(const BeatTracker& tracker, const AudioBuffer& audio,
                             const BeatDetectOptions& options = BeatDetectOptions{}, Progress* progress = nullptr);

} // namespace srt
