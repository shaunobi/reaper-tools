#pragma once
// Stem separation with a host-STFT ONNX model (BS-RoFormer export). The
// model sees one fixed-length chunk at a time as a complex spectrogram;
// this class handles resampling, the transform, chunking with overlap-add,
// and resampling back.

#include "srt/audio.h"
#include "srt/jobs.h"
#include "srt/onnx.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace srt {

struct SeparatorConfig {
    std::filesystem::path modelPath;
    std::vector<Provider> providers = OnnxRuntime::availableProviders();
    int sampleRate = 44100;
    int nFft = 2048;
    int hop = 441;
    double overlap = 0.5; // fraction of a chunk shared with the next
    std::vector<std::string> stemNames = {"drums", "bass", "other", "vocals"};
};

// One chunk in, one chunk out. Layouts are [channel][bin][frame] for the
// input and [stem][channel][bin][frame] for the output, interleaved complex
// as separate real and imaginary arrays.
struct ChunkSpectrum {
    std::vector<float> real;
    std::vector<float> imag;
};
using ChunkModel = std::function<void(const ChunkSpectrum& in, ChunkSpectrum& out)>;

class StemSeparator {
public:
    explicit StemSeparator(SeparatorConfig config);
    ~StemSeparator();

    Provider provider() const;
    const SeparatorConfig& config() const;
    size_t numStems() const;
    size_t chunkSamples() const;
    size_t chunkFrames() const;

    // Any rate, any channel count. Returns one stereo buffer per stem at the
    // input's sample rate and length, in stemNames order.
    std::vector<AudioBuffer> separate(const AudioBuffer& audio, Progress* progress = nullptr) const;

    // The chunking pipeline with an arbitrary model, for tests. Input must be
    // stereo at the model rate; output stems are stereo at the model rate.
    static std::vector<AudioBuffer> separateWith(const AudioBuffer& stereo, const ChunkModel& model, size_t numStems,
                                                 size_t chunkSamples, int nFft, int hop, double overlap,
                                                 Progress* progress = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace srt
