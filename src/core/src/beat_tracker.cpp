#include "srt/beat_tracker.h"

#include "srt/resample.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace srt {

struct BeatTracker::Impl {
    BeatTrackerConfig config;
    LogMelSpectrogram mel;
    OnnxSession session;
    std::string inputName;
    std::string beatName;
    std::string downbeatName;

    explicit Impl(BeatTrackerConfig c) : config(std::move(c)), mel(config.mel) {
        session = OnnxRuntime::createSession(config.modelPath, config.providers);
        Ort::AllocatorWithDefaultOptions alloc;
        if (session.session->GetInputCount() != 1) throw std::runtime_error("beat model: expected one input");
        inputName = session.session->GetInputNameAllocated(0, alloc).get();
        const size_t outputs = session.session->GetOutputCount();
        for (size_t i = 0; i < outputs; ++i) {
            std::string name = session.session->GetOutputNameAllocated(i, alloc).get();
            if (name == "downbeat") downbeatName = name;
            else if (name == "beat") beatName = name;
        }
        if (beatName.empty() || downbeatName.empty())
            throw std::runtime_error("beat model: expected outputs named 'beat' and 'downbeat'");
    }

    // Runs the network on one chunk of `frames` rows. Returns logits.
    void runChunk(const float* spect, size_t frames, std::vector<float>& beat, std::vector<float>& downbeat) {
        const int64_t shape[] = {1, static_cast<int64_t>(frames), static_cast<int64_t>(config.mel.nMels)};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(spect),
                                                           frames * static_cast<size_t>(config.mel.nMels), shape, 3);
        const char* inNames[] = {inputName.c_str()};
        const char* outNames[] = {beatName.c_str(), downbeatName.c_str()};
        auto out = session.session->Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 2);
        auto copyOut = [&](Ort::Value& v, std::vector<float>& dst) {
            auto info = v.GetTensorTypeAndShapeInfo();
            const size_t n = info.GetElementCount();
            if (n != frames) throw std::runtime_error("beat model: unexpected output length");
            const float* p = v.GetTensorData<float>();
            dst.assign(p, p + n);
        };
        copyOut(out[0], beat);
        copyOut(out[1], downbeat);
    }
};

BeatTracker::BeatTracker(BeatTrackerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
BeatTracker::~BeatTracker() = default;
BeatTracker::BeatTracker(BeatTracker&&) noexcept = default;
BeatTracker& BeatTracker::operator=(BeatTracker&&) noexcept = default;

Provider BeatTracker::provider() const { return impl_->session.provider; }
const BeatTrackerConfig& BeatTracker::config() const { return impl_->config; }

BeatActivations BeatTracker::activations(const AudioBuffer& audio, Progress* progress) const {
    if (audio.numChannels() == 0 || audio.numFrames() == 0) throw std::invalid_argument("beat tracker: empty audio");
    std::vector<float> mono = audio.mixToMono();
    if (progress) progress->set(0.02, "resampling");
    mono = resample(mono, audio.sampleRate, impl_->config.mel.sampleRate);
    return activationsFromMono(mono, progress);
}

BeatActivations BeatTracker::activationsFromMono(const std::vector<float>& mono, Progress* progress) const {
    if (progress) progress->set(0.05, "computing spectrogram");
    std::vector<float> spect = impl_->mel.compute(mono.data(), mono.size());
    return activationsFromSpectrogram(spect, impl_->mel.numFrames(mono.size()), progress);
}

BeatActivations BeatTracker::activationsFromSpectrogram(const std::vector<float>& spect, size_t numFrames,
                                                        Progress* progress) const {
    const size_t nMels = static_cast<size_t>(impl_->config.mel.nMels);
    if (spect.size() != numFrames * nMels) throw std::invalid_argument("beat tracker: spectrogram size mismatch");
    const long len = static_cast<long>(numFrames);
    const long chunk = impl_->config.chunkFrames;
    const long border = impl_->config.borderFrames;
    const long step = chunk - 2 * border;

    // Chunk starts, as in beat_this.inference.split_piece with avoid_short_end.
    std::vector<long> starts;
    for (long s = -border; s < len - border; s += step) starts.push_back(s);
    if (starts.empty()) starts.push_back(-border);
    if (len > step) starts.back() = len - (chunk - border);

    BeatActivations act;
    act.beat.assign(numFrames, -1000.0f);
    act.downbeat.assign(numFrames, -1000.0f);

    std::vector<float> chunkSpect, beat, downbeat;
    // keep_first: later chunks are written first so earlier ones overwrite the overlap.
    for (size_t idx = starts.size(); idx-- > 0;) {
        if (progress && progress->cancelled()) throw JobCancelled{};
        const long start = starts[idx];
        const long from = std::max(start, 0L);
        const long to = std::min(start + chunk, len);
        const long padLeft = std::max(0L, -start);
        const long padRight = std::max(0L, std::min(border, start + chunk - len));
        const long frames = padLeft + (to - from) + padRight;

        chunkSpect.assign(static_cast<size_t>(frames) * nMels, 0.0f);
        std::copy(spect.begin() + from * static_cast<long>(nMels), spect.begin() + to * static_cast<long>(nMels),
                  chunkSpect.begin() + padLeft * static_cast<long>(nMels));
        impl_->runChunk(chunkSpect.data(), static_cast<size_t>(frames), beat, downbeat);

        // Discard the border on both sides, then write into the piece.
        const long validFrames = frames - 2 * border;
        const long dstStart = start + border;
        for (long i = 0; i < validFrames; ++i) {
            const long dst = dstStart + i;
            if (dst < 0 || dst >= len) continue;
            act.beat[static_cast<size_t>(dst)] = beat[static_cast<size_t>(border + i)];
            act.downbeat[static_cast<size_t>(dst)] = downbeat[static_cast<size_t>(border + i)];
        }
        if (progress) {
            const double done = static_cast<double>(starts.size() - idx) / static_cast<double>(starts.size());
            progress->set(0.1 + 0.85 * done, "tracking beats");
        }
    }
    return act;
}

namespace {

// Peak frames as in beat_this: local maximum within +/-3 frames and logit > 0.
std::vector<long> peakFrames(const std::vector<float>& logits) {
    std::vector<long> peaks;
    const long n = static_cast<long>(logits.size());
    for (long i = 0; i < n; ++i) {
        const float v = logits[static_cast<size_t>(i)];
        if (!(v > 0.0f)) continue;
        bool isMax = true;
        for (long j = std::max(0L, i - 3); j <= std::min(n - 1, i + 3) && isMax; ++j)
            if (logits[static_cast<size_t>(j)] > v) isMax = false;
        if (isMax) peaks.push_back(i);
    }
    return peaks;
}

// beat_this.model.postprocessor.deduplicate_peaks(width=1): runs of frames at
// most one apart collapse to their running mean.
std::vector<double> deduplicate(const std::vector<long>& peaks) {
    std::vector<double> out;
    if (peaks.empty()) return out;
    double p = static_cast<double>(peaks[0]);
    long last = peaks[0];
    int count = 1;
    for (size_t i = 1; i < peaks.size(); ++i) {
        const long p2 = peaks[i];
        if (p2 - last <= 1) {
            ++count;
            p += (static_cast<double>(p2) - p) / count;
        } else {
            out.push_back(p);
            p = static_cast<double>(p2);
            count = 1;
        }
        last = p2;
    }
    out.push_back(p);
    return out;
}

} // namespace

BeatEvents pickPeaksMinimal(const BeatActivations& act) {
    BeatEvents ev;
    for (double f : deduplicate(peakFrames(act.beat))) ev.beats.push_back(f / act.fps);
    std::vector<double> downbeats;
    for (double f : deduplicate(peakFrames(act.downbeat))) downbeats.push_back(f / act.fps);
    if (!ev.beats.empty()) {
        for (double& d : downbeats) {
            auto it = std::min_element(ev.beats.begin(), ev.beats.end(),
                                       [d](double a, double b) { return std::fabs(a - d) < std::fabs(b - d); });
            d = *it;
        }
    }
    std::sort(downbeats.begin(), downbeats.end());
    downbeats.erase(std::unique(downbeats.begin(), downbeats.end()), downbeats.end());
    ev.downbeats = std::move(downbeats);
    return ev;
}

double estimateTempoBpm(const std::vector<double>& beats) {
    if (beats.size() < 2) return 0.0;
    // Nominal period from the median interval, robust to a few missed beats.
    std::vector<double> intervals;
    intervals.reserve(beats.size() - 1);
    for (size_t i = 1; i < beats.size(); ++i) intervals.push_back(beats[i] - beats[i - 1]);
    std::sort(intervals.begin(), intervals.end());
    const size_t n = intervals.size();
    const double median = (n % 2 == 1) ? intervals[n / 2] : 0.5 * (intervals[n / 2 - 1] + intervals[n / 2]);
    if (median <= 0.0) return 0.0;
    if (beats.size() < 4) return 60.0 / median;

    // Assign each beat an integer index at the nominal period, then fit
    // time = a + period * index by least squares. Averages out the frame grid.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (double t : beats) {
        const double k = std::round((t - beats.front()) / median);
        sx += k;
        sy += t;
        sxx += k * k;
        sxy += k * t;
    }
    const double count = static_cast<double>(beats.size());
    const double denom = count * sxx - sx * sx;
    if (denom <= 0.0) return 60.0 / median;
    const double period = (count * sxy - sx * sy) / denom;
    return period > 0.0 ? 60.0 / period : 60.0 / median;
}

} // namespace srt
