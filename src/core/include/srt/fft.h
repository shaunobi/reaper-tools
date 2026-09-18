#pragma once
// Real-to-complex FFT wrapper over pocketfft. One instance per transform size.

#include <complex>
#include <cstddef>
#include <vector>

namespace srt {

class RealFFT {
public:
    explicit RealFFT(size_t size);
    size_t size() const { return size_; }
    size_t numBins() const { return size_ / 2 + 1; }

    // in: size() samples. out: numBins() complex bins. Unnormalised.
    void forward(const float* in, std::complex<float>* out) const;
    // in: numBins() complex bins. out: size() samples. Scaled by 1/size().
    void inverse(const std::complex<float>* in, float* out) const;

private:
    size_t size_;
};

} // namespace srt
