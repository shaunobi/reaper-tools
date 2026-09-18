#pragma once
// Tempo-constrained decoding of beat/downbeat activations with a dynamic
// Bayesian network, after Böck, Krebs and Widmer ("Joint beat and downbeat
// tracking with recurrent neural networks", ISMIR 2016) as implemented in
// madmom's DBNDownBeatTrackingProcessor. The hidden state is (beat in bar,
// position within beat, tempo). Viterbi decoding finds the most likely
// sequence, so a missed or spurious peak cannot derail the count.

#include "srt/beat_tracker.h"

namespace srt {

struct DbnConfig {
    int beatsPerBar = 4;
    double minBpm = 55.0;
    double maxBpm = 215.0;
    double fps = 50.0;
    double transitionLambda = 100.0; // tempo-change penalty
    int observationLambda = 16; // fraction of a beat treated as "on the beat"
    double threshold = 0.05; // activations below this at both ends are trimmed
};

// A config whose tempo range is bpm * (1 -/+ tolerance).
DbnConfig dbnConfigForTempo(double bpm, double tolerance = 0.15, int beatsPerBar = 4);

// Events in seconds. Downbeats are the beats decoded as beat 1 of the bar.
BeatEvents decodeDbn(const BeatActivations& act, const DbnConfig& config = DbnConfig{});

} // namespace srt
