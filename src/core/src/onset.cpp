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

    // The flux peaks once a good part of the attack is inside the frame that
    // ends at bestPos, so the attack itself started somewhere in that frame.
    // Walk the frame in 1 ms slices and return the first slice whose level
    // rises clearly above the floor just before the frame: that is the
    // transient's start, which is what a stretch marker or snap offset
    // should sit on. (Measured on real overheads, the flux peak alone is
    // 10-25 ms late on cymbal-heavy hits.)
    const long slice = std::max(1L, static_cast<long>(std::lround(0.001 * sampleRate)));
    const long len = static_cast<long>(mono.size());
    auto sliceRms = [&](long start) {
        double acc = 0.0;
        long count = 0;
        for (long i = start; i < start + slice; ++i) {
            if (i < 0 || i >= len) continue;
            acc += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i)];
            ++count;
        }
        return count ? std::sqrt(acc / static_cast<double>(count)) : 0.0;
    };
    // Scan two frames back: a soft attack inside a cymbal wash can peak the
    // flux 15-25 ms after it starts.
    const long frameStart = bestPos - 2 * n;
    // Floor: the quietest-ish level in the 10 ms before the frame (median of slices).
    std::vector<double> before;
    for (long s = frameStart - 10 * slice; s < frameStart; s += slice) before.push_back(sliceRms(s));
    std::nth_element(before.begin(), before.begin() + static_cast<long>(before.size() / 2), before.end());
    const double floor = before.empty() ? 0.0 : before[before.size() / 2];
    // Peak: the loudest slice from the frame start to a little past the flux peak.
    double peak = 0.0;
    for (long s = frameStart; s <= bestPos + n / 2; s += slice) peak = std::max(peak, sliceRms(s));
    if (peak <= floor) return static_cast<double>(bestPos - hop / 2) / sampleRate;
    const double threshold = std::max(floor + 0.25 * (peak - floor), 2.0 * floor);
    for (long s = frameStart; s <= bestPos; s += slice)
        if (sliceRms(s) >= threshold) return static_cast<double>(s) / sampleRate;
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
