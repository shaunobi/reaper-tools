#include "srt/fft.h"

#include "pocketfft_hdronly.h"

#include <stdexcept>

namespace srt {

RealFFT::RealFFT(size_t size) : size_(size) {
    if (size < 2) throw std::invalid_argument("RealFFT size must be at least 2");
}

void RealFFT::forward(const float* in, std::complex<float>* out) const {
    const pocketfft::shape_t shape{size_};
    const pocketfft::stride_t strideIn{static_cast<ptrdiff_t>(sizeof(float))};
    const pocketfft::stride_t strideOut{static_cast<ptrdiff_t>(sizeof(std::complex<float>))};
    pocketfft::r2c(shape, strideIn, strideOut, 0, pocketfft::FORWARD, in, out, 1.0f);
}

void RealFFT::inverse(const std::complex<float>* in, float* out) const {
    const pocketfft::shape_t shape{size_};
    const pocketfft::stride_t strideIn{static_cast<ptrdiff_t>(sizeof(std::complex<float>))};
    const pocketfft::stride_t strideOut{static_cast<ptrdiff_t>(sizeof(float))};
    pocketfft::c2r(shape, strideIn, strideOut, 0, pocketfft::BACKWARD, in, out,
                   1.0f / static_cast<float>(size_));
}

} // namespace srt
