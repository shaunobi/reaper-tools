#pragma once
// Short-time Fourier transform matching torch.stft / torch.istft with
// center=True, reflect padding, a periodic Hann window and no normalisation.
// Frames are stored bin-major per frame: frame t occupies [t*numBins, (t+1)*numBins).

#include <complex>
#include <cstddef>
#include <vector>

namespace srt {

class Stft {
public:
    Stft(size_t nFft, size_t hop);
    size_t nFft() const { return nFft_; }
    size_t hop() const { return hop_; }
    size_t numBins() const { return nFft_ / 2 + 1; }
    // torch: 1 + floor(numSamples / hop) frames when center=True.
    size_t numFrames(size_t numSamples) const { return 1 + numSamples / hop_; }

    // out receives numFrames(n) * numBins() complex values.
    void forward(const float* mono, size_t numSamples, std::vector<std::complex<float>>& out) const;
    // Reconstructs `length` samples from numFrames frames (window-envelope normalised).
    void inverse(const std::complex<float>* frames, size_t numFrames, size_t length, std::vector<float>& out) const;

    const std::vector<float>& window() const { return window_; }

private:
    size_t nFft_;
    size_t hop_;
    std::vector<float> window_;
};

} // namespace srt
