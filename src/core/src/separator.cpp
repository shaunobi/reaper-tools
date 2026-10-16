#include "srt/separator.h"

#include "srt/resample.h"
#include "srt/stft.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace srt {

struct StemSeparator::Impl {
    SeparatorConfig config;
    OnnxSession session;
    std::string inReal, inImag, outReal, outImag;
    size_t bins = 0, frames = 0, stems = 0, chunkSamples = 0;

    explicit Impl(SeparatorConfig c) : config(std::move(c)) {
        session = OnnxRuntime::createSession(config.modelPath, config.providers);
        Ort::AllocatorWithDefaultOptions alloc;
        auto& s = *session.session;
        if (s.GetInputCount() != 2 || s.GetOutputCount() != 2)
            throw std::runtime_error("separation model: expected two inputs and two outputs");
        for (size_t i = 0; i < 2; ++i) {
            std::string name = s.GetInputNameAllocated(i, alloc).get();
            if (name.find("imag") != std::string::npos) inImag = name;
            else inReal = name;
            std::string oname = s.GetOutputNameAllocated(i, alloc).get();
            if (oname.find("imag") != std::string::npos) outImag = oname;
            else outReal = oname;
        }
        auto inShape = s.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape(); // [1, 2, bins, frames]
        auto outShape = s.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape(); // [1, stems, 2, bins, frames]
        if (inShape.size() != 4 || outShape.size() != 5 || inShape[1] != 2 || outShape[2] != 2)
            throw std::runtime_error("separation model: unexpected tensor shapes");
        bins = static_cast<size_t>(inShape[2]);
        frames = static_cast<size_t>(inShape[3]);
        stems = static_cast<size_t>(outShape[1]);
        if (bins != static_cast<size_t>(config.nFft / 2 + 1))
            throw std::runtime_error("separation model: FFT size does not match the model");
        chunkSamples = (frames - 1) * static_cast<size_t>(config.hop);
        if (config.stemNames.size() != stems) {
            config.stemNames.clear();
            for (size_t i = 0; i < stems; ++i) config.stemNames.push_back("stem" + std::to_string(i + 1));
        }
    }

    void run(const ChunkSpectrum& in, ChunkSpectrum& out) {
        const int64_t inShape[] = {1, 2, static_cast<int64_t>(bins), static_cast<int64_t>(frames)};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputs[2] = {
            Ort::Value::CreateTensor<float>(mem, const_cast<float*>(in.real.data()), in.real.size(), inShape, 4),
            Ort::Value::CreateTensor<float>(mem, const_cast<float*>(in.imag.data()), in.imag.size(), inShape, 4)};
        const char* inNames[] = {inReal.c_str(), inImag.c_str()};
        const char* outNames[] = {outReal.c_str(), outImag.c_str()};
        auto result = session.session->Run(Ort::RunOptions{nullptr}, inNames, inputs, 2, outNames, 2);
        const size_t n = stems * 2 * bins * frames;
        if (result[0].GetTensorTypeAndShapeInfo().GetElementCount() != n)
            throw std::runtime_error("separation model: unexpected output size");
        const float* r = result[0].GetTensorData<float>();
        const float* i = result[1].GetTensorData<float>();
        out.real.assign(r, r + n);
        out.imag.assign(i, i + n);
    }
};

StemSeparator::StemSeparator(SeparatorConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
StemSeparator::~StemSeparator() = default;
Provider StemSeparator::provider() const { return impl_->session.provider; }
const SeparatorConfig& StemSeparator::config() const { return impl_->config; }
size_t StemSeparator::numStems() const { return impl_->stems; }
size_t StemSeparator::chunkSamples() const { return impl_->chunkSamples; }
size_t StemSeparator::chunkFrames() const { return impl_->frames; }

std::vector<AudioBuffer> StemSeparator::separateWith(const AudioBuffer& stereo, const ChunkModel& model,
                                                     size_t numStems, size_t chunkSamples, int nFft, int hop,
                                                     double overlap, Progress* progress) {
    if (stereo.numChannels() != 2) throw std::invalid_argument("separateWith: stereo input required");
    const size_t samples = stereo.numFrames();
    const Stft stft(static_cast<size_t>(nFft), static_cast<size_t>(hop));
    const size_t frames = stft.numFrames(chunkSamples);
    const size_t bins = stft.numBins();
    const size_t step = std::max<size_t>(1, static_cast<size_t>(std::llround(chunkSamples * (1.0 - overlap))));

    // Hann weighting across each chunk; accumulate weighted stems and weights.
    // The first chunk keeps a flat start and the last chunk a flat end, so the
    // edges of the file are never attenuated by a near-zero weight.
    std::vector<double> win(chunkSamples);
    for (size_t i = 0; i < chunkSamples; ++i)
        win[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(chunkSamples));
    std::vector<double> winFlatStart(win), winFlatEnd(win), winFlat(chunkSamples, 1.0);
    for (size_t i = 0; i < chunkSamples / 2; ++i) winFlatStart[i] = 1.0;
    for (size_t i = chunkSamples / 2; i < chunkSamples; ++i) winFlatEnd[i] = 1.0;
    const size_t total = samples + chunkSamples;
    std::vector<std::vector<std::vector<double>>> acc(numStems, std::vector<std::vector<double>>(2, std::vector<double>(total, 0.0)));
    std::vector<double> weight(total, 0.0);

    size_t numChunks = 0;
    for (size_t off = 0; off < samples; off += step) ++numChunks;

    std::vector<float> chunk(chunkSamples);
    std::vector<std::complex<float>> spec;
    ChunkSpectrum in, out;
    in.real.resize(2 * bins * frames);
    in.imag.resize(2 * bins * frames);
    std::vector<float> recon;
    std::vector<std::complex<float>> stemSpec(frames * bins);

    size_t done = 0;
    for (size_t off = 0; off < samples; off += step, ++done) {
        if (progress && progress->cancelled()) throw JobCancelled{};
        const size_t len = std::min(chunkSamples, samples - off);
        const bool first = (done == 0), last = (done + 1 == numChunks);
        const std::vector<double>& w = (first && last) ? winFlat : first ? winFlatStart : last ? winFlatEnd : win;
        for (size_t ch = 0; ch < 2; ++ch) {
            std::fill(chunk.begin(), chunk.end(), 0.0f);
            std::copy(stereo.channels[ch].begin() + static_cast<long>(off), stereo.channels[ch].begin() + static_cast<long>(off + len), chunk.begin());
            stft.forward(chunk.data(), chunkSamples, spec); // frame-major
            // Model layout is [channel][bin][frame].
            for (size_t t = 0; t < frames; ++t)
                for (size_t b = 0; b < bins; ++b) {
                    const std::complex<float> v = spec[t * bins + b];
                    in.real[(ch * bins + b) * frames + t] = v.real();
                    in.imag[(ch * bins + b) * frames + t] = v.imag();
                }
        }
        model(in, out);
        if (out.real.size() != numStems * 2 * bins * frames) throw std::runtime_error("separateWith: bad model output");
        for (size_t s = 0; s < numStems; ++s) {
            for (size_t ch = 0; ch < 2; ++ch) {
                const size_t base = (s * 2 + ch) * bins * frames;
                for (size_t t = 0; t < frames; ++t)
                    for (size_t b = 0; b < bins; ++b)
                        stemSpec[t * bins + b] = {out.real[base + b * frames + t], out.imag[base + b * frames + t]};
                stft.inverse(stemSpec.data(), frames, chunkSamples, recon);
                double* dst = acc[s][ch].data() + off;
                for (size_t i = 0; i < chunkSamples; ++i) dst[i] += recon[i] * w[i];
            }
        }
        for (size_t i = 0; i < chunkSamples; ++i) weight[off + i] += w[i];
        if (progress) progress->set(0.05 + 0.85 * static_cast<double>(done + 1) / static_cast<double>(numChunks), "separating");
    }

    std::vector<AudioBuffer> stems;
    for (size_t s = 0; s < numStems; ++s) {
        AudioBuffer b(2, samples, stereo.sampleRate);
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < samples; ++i)
                b.channels[ch][i] = static_cast<float>(acc[s][ch][i] / std::max(weight[i], 1e-8));
        stems.push_back(std::move(b));
    }
    return stems;
}

std::vector<AudioBuffer> StemSeparator::separate(const AudioBuffer& audio, Progress* progress) const {
    if (audio.numChannels() == 0 || audio.numFrames() == 0) throw std::invalid_argument("separate: empty audio");
    const double inRate = audio.sampleRate;
    const size_t inFrames = audio.numFrames();

    AudioBuffer stereo;
    stereo.sampleRate = inRate;
    if (audio.numChannels() == 1) stereo.channels = {audio.channels[0], audio.channels[0]};
    else stereo.channels = {audio.channels[0], audio.channels[1]};
    if (progress) progress->set(0.02, "resampling");
    stereo = resample(stereo, impl_->config.sampleRate);

    Impl* impl = impl_.get();
    std::vector<AudioBuffer> stems = separateWith(
        stereo, [impl](const ChunkSpectrum& in, ChunkSpectrum& out) { impl->run(in, out); }, impl_->stems,
        impl_->chunkSamples, impl_->config.nFft, impl_->config.hop, impl_->config.overlap, progress);

    if (progress) progress->set(0.92, "resampling stems");
    for (auto& s : stems) {
        s = resample(s, inRate);
        for (auto& ch : s.channels) ch.resize(inFrames, 0.0f);
    }
    if (progress) progress->set(1.0, "done");
    return stems;
}

} // namespace srt
