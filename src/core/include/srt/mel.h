#pragma once
// Log-mel spectrogram front end matching torchaudio.transforms.MelSpectrogram
// as configured by Beat This!: centred frames with reflect padding, periodic
// Hann window, magnitude divided by sqrt(n_fft), Slaney mel filterbank with
// no normalisation, then log1p(logMultiplier * mel).

#include <cstddef>
#include <vector>

namespace srt {

struct LogMelConfig {
    int sampleRate = 22050;
    int nFft = 1024;
    int hop = 441;
    int nMels = 128;
    double fMin = 30.0;
    double fMax = 11000.0;
    double logMultiplier = 1000.0;
};

class LogMelSpectrogram {
public:
    explicit LogMelSpectrogram(LogMelConfig config = LogMelConfig{});

    const LogMelConfig& config() const { return config_; }
    size_t numBins() const { return static_cast<size_t>(config_.nFft / 2 + 1); }
    size_t numFrames(size_t numSamples) const { return 1 + numSamples / static_cast<size_t>(config_.hop); }

    // mono: at least nFft/2 + 1 samples. Returns numFrames(n) * nMels values,
    // row-major (frame, mel).
    std::vector<float> compute(const float* mono, size_t numSamples) const;

    // nMels x numBins, row-major. Same values as torchaudio.functional.melscale_fbanks
    // with norm=None and mel_scale="slaney", transposed.
    const std::vector<float>& filterbank() const { return filterbank_; }

    static std::vector<double> slaneyFilterbank(int nFft, int sampleRate, int nMels, double fMin, double fMax);

private:
    LogMelConfig config_;
    std::vector<float> window_;
    std::vector<float> filterbank_;
};

} // namespace srt
