#include "srt/onset.h"

#include "srt/fft.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace srt {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Log-compressed magnitude spectrum of the frame ending at `end` (exclusive).
// Frames that run past the signal edges are zero padded.
void frameSpectrum(const std::vector<float>& mono, long end, const std::vector<float>& window, const RealFFT& fft,
                   std::vector<float>& scratch, std::vector<std::complex<float>>& bins, std::vector<float>& mag) {
    const long n = static_cast<long>(window.size());
    const long start = end - n;
    const long len = static_cast<long>(mono.size());
    for (long i = 0; i < n; ++i) {
        const long idx = start + i;
        const float s = (idx >= 0 && idx < len) ? mono[static_cast<size_t>(idx)] : 0.0f;
        scratch[static_cast<size_t>(i)] = s * window[static_cast<size_t>(i)];
    }
    fft.forward(scratch.data(), bins.data());
    for (size_t k = 0; k < bins.size(); ++k) mag[k] = std::log1p(1000.0f * std::abs(bins[k]));
}

} // namespace

double refineOnset(const std::vector<float>& mono, double sampleRate, double time, const OnsetRefineConfig& config) {
    if (mono.empty() || sampleRate <= 0.0) return time;
    const long hop = std::max(1L, static_cast<long>(std::lround(config.hopSeconds * sampleRate)));
    const long half = static_cast<long>(std::lround(config.windowSeconds * sampleRate));
    const long centre = static_cast<long>(std::lround(time * sampleRate));
    const long n = config.fftSize;

    RealFFT fft(static_cast<size_t>(n));
    std::vector<float> window(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) window[static_cast<size_t>(i)] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / n)));
    std::vector<float> scratch(static_cast<size_t>(n));
    std::vector<std::complex<float>> bins(fft.numBins());
    std::vector<float> prev(fft.numBins()), cur(fft.numBins());

    // Spectral flux at candidate t compares the frame ending at t with the
    // frame ending one hop earlier, so the flux peaks right as the hit arrives.
    long bestPos = centre;
    float bestFlux = 0.0f;
    const long first = centre - half;
    frameSpectrum(mono, first - hop, window, fft, scratch, bins, prev);
    for (long pos = first; pos <= centre + half; pos += hop) {
        frameSpectrum(mono, pos, window, fft, scratch, bins, cur);
        float flux = 0.0f;
        for (size_t k = 0; k < cur.size(); ++k) flux += std::max(0.0f, cur[k] - prev[k]);
        if (flux > bestFlux) {
            bestFlux = flux;
            bestPos = pos;
        }
        std::swap(prev, cur);
    }
    if (bestFlux <= 0.0f) return time;
    // The flux frame ends at bestPos; the attack sits inside the last hop of it.
    return static_cast<double>(bestPos - hop / 2) / sampleRate;
}

std::vector<double> refineOnsets(const std::vector<float>& mono, double sampleRate, const std::vector<double>& times,
                                 const OnsetRefineConfig& config) {
    std::vector<double> out;
    out.reserve(times.size());
    for (double t : times) out.push_back(refineOnset(mono, sampleRate, t, config));
    return out;
}

} // namespace srt
