#pragma once
// Beat and downbeat tracking with the Beat This! model (Foscarin, Schlüter,
// Widmer, ISMIR 2024) running in ONNX Runtime. Reproduces the reference
// implementation's front end, chunking and "minimal" peak picking.

#include "srt/audio.h"
#include "srt/jobs.h"
#include "srt/mel.h"
#include "srt/onnx.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace srt {

struct BeatTrackerConfig {
    std::filesystem::path modelPath;
    std::vector<Provider> providers = OnnxRuntime::availableProviders();
    LogMelConfig mel;
    int chunkFrames = 1500; // the model's training length
    int borderFrames = 6; // frames discarded at chunk edges
};

// Framewise logits at 50 frames per second.
struct BeatActivations {
    std::vector<float> beat;
    std::vector<float> downbeat;
    double fps = 50.0;
    size_t numFrames() const { return beat.size(); }
};

// Event times in seconds.
struct BeatEvents {
    std::vector<double> beats;
    std::vector<double> downbeats;
};

class BeatTracker {
public:
    explicit BeatTracker(BeatTrackerConfig config);
    ~BeatTracker();
    BeatTracker(BeatTracker&&) noexcept;
    BeatTracker& operator=(BeatTracker&&) noexcept;

    Provider provider() const;
    const BeatTrackerConfig& config() const;

    // Any rate, any channel count: downmixed and resampled internally.
    BeatActivations activations(const AudioBuffer& audio, Progress* progress = nullptr) const;
    // Mono audio already at the model rate.
    BeatActivations activationsFromMono(const std::vector<float>& mono, Progress* progress = nullptr) const;
    // Spectrogram of numFrames rows, nMels columns.
    BeatActivations activationsFromSpectrogram(const std::vector<float>& spect, size_t numFrames,
                                               Progress* progress = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The reference "minimal" postprocessor: local maxima within +/-3 frames with
// logit > 0, adjacent peaks merged, downbeats snapped to the nearest beat.
BeatEvents pickPeaksMinimal(const BeatActivations& act);

// Tempo from a least-squares fit of beat times against beat index (median
// interval as the starting guess). Returns 0 with fewer than two beats.
double estimateTempoBpm(const std::vector<double>& beats);

} // namespace srt
