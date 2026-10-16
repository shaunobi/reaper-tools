#pragma once
// Stem separation with Demucs v4 fine-tuned (htdemucs_ft): four HTDemucs
// models, one specialised per stem, exported to ONNX with the spectrogram and
// its inverse outside the graph (tools/export_demucs.py). This class does
// what demucs.apply.apply_model does around the network: 7.8 s segments
// with 25% overlap and triangular weights, the centred zero padding of the
// last segment, the host STFT/iSTFT with Demucs' own padding rules, and the
// sum of the spectral and time branches. shifts=0 (deterministic).

#include "srt/audio.h"
#include "srt/jobs.h"
#include "srt/onnx.h"
#include "srt/stft.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace srt {

struct DemucsConfig {
    // One model per stem, in stemNames order (the bag's weights pick stem i
    // from model i). A single path works too: every stem then comes from it.
    std::vector<std::filesystem::path> modelPaths;
    std::vector<std::string> stemNames = {"drums", "bass", "other", "vocals"};
    std::vector<Provider> providers = OnnxRuntime::availableProviders();
    int sampleRate = 44100;
    int nFft = 4096;
    int hop = 1024;
    double overlap = 0.25;
};

// One segment in: the raw stereo waveform [2][L] and its host spectrogram
// [2][bins][frames] (real and imaginary). One segment out, already combined
// across the bag: spectral output [stem][2][bins][frames] and time-branch
// output [stem][2][L], both de-normalised by the graph.
struct DemucsSegmentIn {
    std::vector<float> mix;
    std::vector<float> specReal, specImag;
    // The slice of the overall progress this segment covers, so a model
    // running several passes can report inside it.
    double progressFrom = 0.0, progressTo = 0.0;
};
struct DemucsSegmentOut {
    std::vector<float> specReal, specImag;
    std::vector<float> time;
};
using DemucsModel = std::function<void(const DemucsSegmentIn& in, DemucsSegmentOut& out)>;

class DemucsSeparator {
public:
    explicit DemucsSeparator(DemucsConfig config);
    ~DemucsSeparator();

    Provider provider() const;
    const DemucsConfig& config() const;
    size_t numStems() const;
    size_t segmentSamples() const;
    size_t bins() const; // nFft / 2 (the last bin is dropped, as in HTDemucs)
    size_t frames() const; // ceil(segment / hop)

    // Any rate, any channel count. Returns one stereo buffer per stem at the
    // input's rate and length, in stemNames order.
    std::vector<AudioBuffer> separate(const AudioBuffer& audio, Progress* progress = nullptr) const;

    // The segmenting pipeline with an arbitrary model, for tests. Input must
    // be stereo at the model rate.
    static std::vector<AudioBuffer> separateWith(const AudioBuffer& stereo, const DemucsModel& model, size_t numStems,
                                                 size_t segmentSamples, int nFft, int hop, double overlap,
                                                 Progress* progress = nullptr);

    // HTDemucs._spec: reflect-pads so the frame count is ceil(length / hop),
    // normalised STFT, last bin dropped. Output [bins][frames], bin-major.
    static void spec(const Stft& stft, const float* mono, size_t length, std::vector<float>& real,
                     std::vector<float>& imag);
    // HTDemucs._ispec: the inverse of spec(), back to `length` samples.
    static void ispec(const Stft& stft, const float* real, const float* imag, size_t length, std::vector<float>& out);
    static size_t specFrames(size_t length, int hop) { return (length + static_cast<size_t>(hop) - 1) / static_cast<size_t>(hop); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace srt
