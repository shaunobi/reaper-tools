#include "srt/mel.h"

#include "srt/fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace srt {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Slaney mel scale, as in librosa and torchaudio (mel_scale="slaney").
double hzToMel(double hz) {
    const double fSp = 200.0 / 3.0;
    const double minLogHz = 1000.0;
    const double minLogMel = minLogHz / fSp;
    const double logStep = std::log(6.4) / 27.0;
    if (hz >= minLogHz) return minLogMel + std::log(hz / minLogHz) / logStep;
    return hz / fSp;
}

double melToHz(double mel) {
    const double fSp = 200.0 / 3.0;
    const double minLogHz = 1000.0;
    const double minLogMel = minLogHz / fSp;
    const double logStep = std::log(6.4) / 27.0;
    if (mel >= minLogMel) return minLogHz * std::exp(logStep * (mel - minLogMel));
    return fSp * mel;
}

} // namespace

std::vector<double> LogMelSpectrogram::slaneyFilterbank(int nFft, int sampleRate, int nMels, double fMin,
                                                        double fMax) {
    const int nFreqs = nFft / 2 + 1;
    std::vector<double> allFreqs(nFreqs);
    for (int k = 0; k < nFreqs; ++k) allFreqs[k] = (sampleRate / 2.0) * k / (nFreqs - 1);

    const double mMin = hzToMel(fMin), mMax = hzToMel(fMax);
    std::vector<double> fPts(nMels + 2);
    for (int i = 0; i < nMels + 2; ++i) fPts[i] = melToHz(mMin + (mMax - mMin) * i / (nMels + 1));

    std::vector<double> fb(static_cast<size_t>(nMels) * nFreqs, 0.0);
    for (int m = 0; m < nMels; ++m) {
        const double lo = fPts[m], mid = fPts[m + 1], hi = fPts[m + 2];
        for (int k = 0; k < nFreqs; ++k) {
            const double f = allFreqs[k];
            const double down = (f - lo) / (mid - lo);
            const double up = (hi - f) / (hi - mid);
            fb[static_cast<size_t>(m) * nFreqs + k] = std::max(0.0, std::min(down, up));
        }
    }
    return fb;
}

LogMelSpectrogram::LogMelSpectrogram(LogMelConfig config) : config_(config) {
    if (config_.nFft <= 0 || config_.hop <= 0 || config_.nMels <= 0)
        throw std::invalid_argument("LogMelSpectrogram: invalid configuration");
    window_.resize(config_.nFft);
    for (int i = 0; i < config_.nFft; ++i)
        window_[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / config_.nFft)));
    std::vector<double> fb = slaneyFilterbank(config_.nFft, config_.sampleRate, config_.nMels, config_.fMin, config_.fMax);
    filterbank_.assign(fb.begin(), fb.end());
}

std::vector<float> LogMelSpectrogram::compute(const float* mono, size_t numSamples) const {
    const size_t nFft = static_cast<size_t>(config_.nFft);
    const size_t hop = static_cast<size_t>(config_.hop);
    const size_t pad = nFft / 2;
    if (numSamples <= pad) throw std::invalid_argument("LogMelSpectrogram: input too short");

    // Reflect padding: [x[pad], ..., x[1], x[0], x[1], ..., x[n-1], x[n-2], ..., x[n-1-pad]]
    std::vector<float> padded(numSamples + 2 * pad);
    for (size_t i = 0; i < pad; ++i) padded[i] = mono[pad - i];
    std::copy(mono, mono + numSamples, padded.begin() + static_cast<std::ptrdiff_t>(pad));
    for (size_t i = 0; i < pad; ++i) padded[pad + numSamples + i] = mono[numSamples - 2 - i];

    const size_t frames = numFrames(numSamples);
    const size_t bins = numBins();
    const size_t nMels = static_cast<size_t>(config_.nMels);
    RealFFT fft(nFft);
    std::vector<float> frame(nFft);
    std::vector<std::complex<float>> spectrum(bins);
    std::vector<float> magnitude(bins);
    std::vector<float> out(frames * nMels);
    const float norm = 1.0f / std::sqrt(static_cast<float>(nFft));

    for (size_t t = 0; t < frames; ++t) {
        const float* src = padded.data() + t * hop;
        for (size_t i = 0; i < nFft; ++i) frame[i] = src[i] * window_[i];
        fft.forward(frame.data(), spectrum.data());
        for (size_t k = 0; k < bins; ++k) magnitude[k] = std::abs(spectrum[k]) * norm;
        float* row = out.data() + t * nMels;
        for (size_t m = 0; m < nMels; ++m) {
            const float* fb = filterbank_.data() + m * bins;
            double acc = 0.0;
            for (size_t k = 0; k < bins; ++k) acc += static_cast<double>(fb[k]) * magnitude[k];
            row[m] = static_cast<float>(std::log1p(config_.logMultiplier * acc));
        }
    }
    return out;
}

} // namespace srt
