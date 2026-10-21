#include "srt/stft.h"

#include "srt/fft.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace srt {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Stft::Stft(size_t nFft, size_t hop) : nFft_(nFft), hop_(hop) {
    if (nFft < 4 || hop == 0 || hop > nFft) throw std::invalid_argument("Stft: invalid sizes");
    window_.resize(nFft);
    for (size_t i = 0; i < nFft; ++i)
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(nFft)));
}

void Stft::forward(const float* mono, size_t numSamples, std::vector<std::complex<float>>& out) const {
    const size_t pad = nFft_ / 2;
    if (numSamples <= pad) throw std::invalid_argument("Stft::forward: input too short");
    const size_t frames = numFrames(numSamples);
    const size_t bins = numBins();
    out.resize(frames * bins);

    RealFFT fft(nFft_);
    std::vector<float> frame(nFft_);
    // Reflect-padded index without materialising the padded signal.
    auto sample = [&](long idx) -> float {
        if (idx < 0) idx = -idx;
        const long n = static_cast<long>(numSamples);
        if (idx >= n) idx = 2 * n - 2 - idx;
        return mono[static_cast<size_t>(idx)];
    };
    for (size_t t = 0; t < frames; ++t) {
        const long start = static_cast<long>(t * hop_) - static_cast<long>(pad);
        for (size_t i = 0; i < nFft_; ++i) frame[i] = sample(start + static_cast<long>(i)) * window_[i];
        fft.forward(frame.data(), out.data() + t * bins);
    }
}

void Stft::inverse(const std::complex<float>* frames, size_t numFrames, size_t length, std::vector<float>& out) const {
    const size_t pad = nFft_ / 2;
    const size_t bins = numBins();
    const size_t padded = (numFrames - 1) * hop_ + nFft_;
    std::vector<double> acc(padded, 0.0), env(padded, 0.0);
    RealFFT fft(nFft_);
    std::vector<float> time(nFft_);
    for (size_t t = 0; t < numFrames; ++t) {
        fft.inverse(frames + t * bins, time.data());
        const size_t base = t * hop_;
        for (size_t i = 0; i < nFft_; ++i) {
            acc[base + i] += static_cast<double>(time[i]) * window_[i];
            env[base + i] += static_cast<double>(window_[i]) * window_[i];
        }
    }
    out.assign(length, 0.0f);
    for (size_t i = 0; i < length; ++i) {
        const size_t j = pad + i;
        if (j >= padded) break;
        out[i] = env[j] > 1e-8 ? static_cast<float>(acc[j] / env[j]) : 0.0f;
    }
}

} // namespace srt
