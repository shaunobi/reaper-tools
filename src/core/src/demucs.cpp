#include "srt/demucs.h"

#include "srt/resample.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace srt {

namespace {

// pad1d(..., mode="reflect") for a signal much longer than the padding.
inline float reflectAt(const float* x, long n, long i) {
    if (i < 0) i = -i;
    if (i >= n) i = 2 * n - 2 - i;
    return x[i];
}

} // namespace

// ---- host transform ---------------------------------------------------------

void DemucsSeparator::spec(const Stft& stft, const float* mono, size_t length, std::vector<float>& real,
                           std::vector<float>& imag) {
    const size_t hl = stft.hop();
    const size_t le = specFrames(length, static_cast<int>(hl));
    const size_t pad = hl / 2 * 3;
    const size_t padRight = pad + le * hl - length;
    const size_t padded = length + pad + padRight; // = 2 * pad + le * hl
    std::vector<float> x(padded);
    const long n = static_cast<long>(length);
    for (size_t i = 0; i < padded; ++i) x[i] = reflectAt(mono, n, static_cast<long>(i) - static_cast<long>(pad));

    std::vector<std::complex<float>> z;
    stft.forward(x.data(), padded, z); // frame-major, le + 4 frames
    const size_t allBins = stft.numBins();
    const size_t bins = allBins - 1;
    if (stft.numFrames(padded) != le + 4) throw std::runtime_error("DemucsSeparator::spec: frame count mismatch");
    const float scale = 1.0f / std::sqrt(static_cast<float>(stft.nFft())); // torch.stft(normalized=True)
    real.assign(bins * le, 0.0f);
    imag.assign(bins * le, 0.0f);
    for (size_t t = 0; t < le; ++t)
        for (size_t b = 0; b < bins; ++b) {
            const std::complex<float> v = z[(t + 2) * allBins + b] * scale;
            real[b * le + t] = v.real();
            imag[b * le + t] = v.imag();
        }
}

void DemucsSeparator::ispec(const Stft& stft, const float* real, const float* imag, size_t length,
                            std::vector<float>& out) {
    const size_t hl = stft.hop();
    const size_t le = specFrames(length, static_cast<int>(hl));
    const size_t pad = hl / 2 * 3;
    const size_t allBins = stft.numBins();
    const size_t bins = allBins - 1;
    const size_t frames = le + 4;
    std::vector<std::complex<float>> z(frames * allBins, std::complex<float>(0.0f, 0.0f));
    const float scale = std::sqrt(static_cast<float>(stft.nFft())); // undo normalized=True
    for (size_t t = 0; t < le; ++t)
        for (size_t b = 0; b < bins; ++b) z[(t + 2) * allBins + b] = std::complex<float>(real[b * le + t], imag[b * le + t]) * scale;
    const size_t padded = hl * le + 2 * pad;
    std::vector<float> full;
    stft.inverse(z.data(), frames, padded, full);
    out.assign(full.begin() + static_cast<long>(pad), full.begin() + static_cast<long>(pad + length));
}

// ---- ONNX bag ---------------------------------------------------------------

struct DemucsSeparator::Impl {
    DemucsConfig config;
    std::vector<OnnxSession> sessions; // one per stem (may alias one model path)
    std::vector<size_t> sessionForStem;
    size_t segment = 0, bins = 0, frames = 0, stems = 0;

    explicit Impl(DemucsConfig c) : config(std::move(c)) {
        if (config.modelPaths.empty()) throw std::invalid_argument("DemucsSeparator: no model paths");
        stems = config.stemNames.size();
        if (config.modelPaths.size() != 1 && config.modelPaths.size() != stems)
            throw std::invalid_argument("DemucsSeparator: need one model per stem, or a single model");
        for (size_t i = 0; i < config.modelPaths.size(); ++i) {
            OnnxSession s = OnnxRuntime::createSession(config.modelPaths[i], config.providers);
            inspect(*s.session, i == 0);
            sessions.push_back(std::move(s));
        }
        for (size_t s = 0; s < stems; ++s) sessionForStem.push_back(config.modelPaths.size() == 1 ? 0 : s);
    }

    void inspect(Ort::Session& s, bool first) {
        if (s.GetInputCount() != 3 || s.GetOutputCount() != 3)
            throw std::runtime_error("Demucs model: expected three inputs and three outputs");
        auto mixShape = s.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape(); // [1, 2, L]
        auto specShape = s.GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape(); // [1, 2, F, T]
        auto outShape = s.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape(); // [1, S, 2, F, T]
        if (mixShape.size() != 3 || specShape.size() != 4 || outShape.size() != 5 || mixShape[1] != 2 || specShape[1] != 2)
            throw std::runtime_error("Demucs model: unexpected tensor shapes");
        const size_t seg = static_cast<size_t>(mixShape[2]), b = static_cast<size_t>(specShape[2]),
                     t = static_cast<size_t>(specShape[3]), st = static_cast<size_t>(outShape[1]);
        if (first) {
            segment = seg;
            bins = b;
            frames = t;
        } else if (seg != segment || b != bins || t != frames) {
            throw std::runtime_error("Demucs model: the models in the bag disagree on sizes");
        }
        if (st != stems) throw std::runtime_error("Demucs model: stem count does not match the configured names");
        if (bins != static_cast<size_t>(config.nFft / 2)) throw std::runtime_error("Demucs model: FFT size does not match");
        if (frames != specFrames(segment, config.hop)) throw std::runtime_error("Demucs model: hop does not match");
    }

    void run(const DemucsSegmentIn& in, DemucsSegmentOut& out, Progress* progress) {
        const double p0 = in.progressFrom, p1 = in.progressTo;
        const int64_t mixShape[] = {1, 2, static_cast<int64_t>(segment)};
        const int64_t specShape[] = {1, 2, static_cast<int64_t>(bins), static_cast<int64_t>(frames)};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const size_t specPerStem = 2 * bins * frames, timePerStem = 2 * segment;
        out.specReal.assign(stems * specPerStem, 0.0f);
        out.specImag.assign(stems * specPerStem, 0.0f);
        out.time.assign(stems * timePerStem, 0.0f);
        const char* inNames[] = {"mix", "spec_real", "spec_imag"};
        const char* outNames[] = {"out_spec_real", "out_spec_imag", "out_time"};
        for (size_t s = 0; s < stems; ++s) {
            if (progress && progress->cancelled()) throw JobCancelled{};
            Ort::Value inputs[3] = {
                Ort::Value::CreateTensor<float>(mem, const_cast<float*>(in.mix.data()), in.mix.size(), mixShape, 3),
                Ort::Value::CreateTensor<float>(mem, const_cast<float*>(in.specReal.data()), in.specReal.size(), specShape, 4),
                Ort::Value::CreateTensor<float>(mem, const_cast<float*>(in.specImag.data()), in.specImag.size(), specShape, 4)};
            auto& session = *sessions[sessionForStem[s]].session;
            auto result = session.Run(Ort::RunOptions{nullptr}, inNames, inputs, 3, outNames, 3);
            if (result[0].GetTensorTypeAndShapeInfo().GetElementCount() != stems * specPerStem ||
                result[2].GetTensorTypeAndShapeInfo().GetElementCount() != stems * timePerStem)
                throw std::runtime_error("Demucs model: unexpected output size");
            // The bag's weights: stem s comes from model s only.
            const float* r = result[0].GetTensorData<float>() + s * specPerStem;
            const float* i = result[1].GetTensorData<float>() + s * specPerStem;
            const float* t = result[2].GetTensorData<float>() + s * timePerStem;
            std::copy(r, r + specPerStem, out.specReal.begin() + static_cast<long>(s * specPerStem));
            std::copy(i, i + specPerStem, out.specImag.begin() + static_cast<long>(s * specPerStem));
            std::copy(t, t + timePerStem, out.time.begin() + static_cast<long>(s * timePerStem));
            if (progress) progress->set(p0 + (p1 - p0) * static_cast<double>(s + 1) / static_cast<double>(stems), "separating");
        }
    }
};

DemucsSeparator::DemucsSeparator(DemucsConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
DemucsSeparator::~DemucsSeparator() = default;
Provider DemucsSeparator::provider() const { return impl_->sessions.front().provider; }
const DemucsConfig& DemucsSeparator::config() const { return impl_->config; }
size_t DemucsSeparator::numStems() const { return impl_->stems; }
size_t DemucsSeparator::segmentSamples() const { return impl_->segment; }
size_t DemucsSeparator::bins() const { return impl_->bins; }
size_t DemucsSeparator::frames() const { return impl_->frames; }

// ---- segmenting (demucs.apply.apply_model, split=True, shifts=0) ------------

std::vector<AudioBuffer> DemucsSeparator::separateWith(const AudioBuffer& stereo, const DemucsModel& model,
                                                       size_t numStems, size_t segment, int nFft, int hop,
                                                       double overlap, Progress* progress) {
    if (stereo.numChannels() != 2) throw std::invalid_argument("DemucsSeparator::separateWith: stereo input required");
    const size_t length = stereo.numFrames();
    if (length == 0) throw std::invalid_argument("DemucsSeparator::separateWith: empty input");
    const Stft stft(static_cast<size_t>(nFft), static_cast<size_t>(hop));
    const size_t bins = stft.numBins() - 1;
    const size_t frames = specFrames(segment, hop);
    const size_t stride = std::max<size_t>(1, static_cast<size_t>((1.0 - overlap) * static_cast<double>(segment)));

    // Triangular weight peaking mid-segment, normalised to 1 (transition_power 1).
    std::vector<double> weight(segment);
    const size_t half = segment / 2;
    for (size_t i = 0; i < segment; ++i) weight[i] = i < half ? static_cast<double>(i + 1) : static_cast<double>(segment - i);
    for (double& w : weight) w /= static_cast<double>(half);

    std::vector<std::vector<std::vector<double>>> acc(numStems, std::vector<std::vector<double>>(2, std::vector<double>(length, 0.0)));
    std::vector<double> sumWeight(length, 0.0);

    size_t numChunks = 0;
    for (size_t off = 0; off < length; off += stride) ++numChunks;

    DemucsSegmentIn in;
    DemucsSegmentOut out;
    in.mix.resize(2 * segment);
    in.specReal.resize(2 * bins * frames);
    in.specImag.resize(2 * bins * frames);
    std::vector<float> real, imag, recon;

    size_t done = 0;
    for (size_t off = 0; off < length; off += stride, ++done) {
        if (progress && progress->cancelled()) throw JobCancelled{};
        const size_t chunkLen = std::min(segment, length - off);
        // TensorChunk.padded: centre the short chunk in the segment, zeros around it.
        const size_t delta = segment - chunkLen;
        const long start = static_cast<long>(off) - static_cast<long>(delta / 2);
        for (size_t ch = 0; ch < 2; ++ch) {
            float* dst = in.mix.data() + ch * segment;
            const auto& src = stereo.channels[ch];
            for (size_t i = 0; i < segment; ++i) {
                const long j = start + static_cast<long>(i);
                dst[i] = (j >= 0 && j < static_cast<long>(length)) ? src[static_cast<size_t>(j)] : 0.0f;
            }
            spec(stft, dst, segment, real, imag);
            std::copy(real.begin(), real.end(), in.specReal.begin() + static_cast<long>(ch * bins * frames));
            std::copy(imag.begin(), imag.end(), in.specImag.begin() + static_cast<long>(ch * bins * frames));
        }
        in.progressFrom = 0.05 + 0.85 * static_cast<double>(done) / static_cast<double>(numChunks);
        in.progressTo = 0.05 + 0.85 * static_cast<double>(done + 1) / static_cast<double>(numChunks);
        model(in, out);
        if (out.specReal.size() != numStems * 2 * bins * frames || out.time.size() != numStems * 2 * segment)
            throw std::runtime_error("DemucsSeparator::separateWith: bad model output");
        for (size_t s = 0; s < numStems; ++s)
            for (size_t ch = 0; ch < 2; ++ch) {
                const size_t specBase = (s * 2 + ch) * bins * frames;
                ispec(stft, out.specReal.data() + specBase, out.specImag.data() + specBase, segment, recon);
                const float* time = out.time.data() + (s * 2 + ch) * segment;
                // center_trim to the chunk, then weighted accumulation.
                double* dst = acc[s][ch].data() + off;
                for (size_t i = 0; i < chunkLen; ++i) {
                    const size_t k = i + delta / 2;
                    dst[i] += (static_cast<double>(recon[k]) + static_cast<double>(time[k])) * weight[i];
                }
            }
        for (size_t i = 0; i < chunkLen; ++i) sumWeight[off + i] += weight[i];
        if (progress) progress->set(in.progressTo, "separating");
    }

    std::vector<AudioBuffer> stems;
    for (size_t s = 0; s < numStems; ++s) {
        AudioBuffer b(2, length, stereo.sampleRate);
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < length; ++i) b.channels[ch][i] = static_cast<float>(acc[s][ch][i] / std::max(sumWeight[i], 1e-12));
        stems.push_back(std::move(b));
    }
    return stems;
}

std::vector<AudioBuffer> DemucsSeparator::separate(const AudioBuffer& audio, Progress* progress) const {
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
        stereo, [impl, progress](const DemucsSegmentIn& in, DemucsSegmentOut& out) { impl->run(in, out, progress); },
        impl_->stems, impl_->segment, impl_->config.nFft, impl_->config.hop, impl_->config.overlap, progress);

    if (progress) progress->set(0.9, "resampling stems");
    for (auto& s : stems) {
        s = resample(s, inRate);
        for (auto& ch : s.channels) ch.resize(inFrames, 0.0f);
    }
    // The caller reports what follows (writing files, "done").
    if (progress) progress->set(0.92, "separated");
    return stems;
}

} // namespace srt
