#include "srt/beat_dbn.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace srt {

namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

// Bar state space: for every tempo interval v (frames per beat) there are v
// positions per beat, replicated for each beat of the bar.
struct StateSpace {
    int beatsPerBar = 4;
    std::vector<int> intervals; // frames per beat, ascending
    int statesPerBeat = 0;
    int numStates = 0;
    std::vector<double> position; // 0 <= position < beatsPerBar
    std::vector<int> interval; // tempo of each state
    std::vector<std::vector<int>> firstStates; // [beat][tempo index]
    std::vector<std::vector<int>> lastStates;

    StateSpace(int beats, int minInterval, int maxInterval) : beatsPerBar(beats) {
        for (int v = minInterval; v <= maxInterval; ++v) intervals.push_back(v);
        for (int v : intervals) statesPerBeat += v;
        numStates = beatsPerBar * statesPerBeat;
        position.resize(numStates);
        interval.resize(numStates);
        firstStates.assign(beatsPerBar, std::vector<int>(intervals.size()));
        lastStates.assign(beatsPerBar, std::vector<int>(intervals.size()));
        for (int b = 0; b < beatsPerBar; ++b) {
            int idx = b * statesPerBeat;
            for (size_t ti = 0; ti < intervals.size(); ++ti) {
                const int v = intervals[ti];
                firstStates[b][ti] = idx;
                lastStates[b][ti] = idx + v - 1;
                for (int j = 0; j < v; ++j) {
                    position[idx] = b + static_cast<double>(j) / v;
                    interval[idx] = v;
                    ++idx;
                }
            }
        }
    }
};

// Sparse transition model in compressed form: for each destination state,
// the list of (source state, log probability).
struct Transitions {
    std::vector<uint32_t> rowStart; // numStates + 1
    std::vector<uint32_t> source;
    std::vector<double> logProb;

    Transitions(const StateSpace& ss, double lambda) {
        std::vector<std::vector<std::pair<uint32_t, double>>> rows(ss.numStates);
        // Within a beat the position advances by one frame with probability 1.
        std::vector<char> isFirst(ss.numStates, 0);
        for (const auto& fs : ss.firstStates)
            for (int s : fs) isFirst[s] = 1;
        for (int s = 0; s < ss.numStates; ++s)
            if (!isFirst[s]) rows[s].push_back({static_cast<uint32_t>(s - 1), 0.0});
        // At a beat boundary the tempo may change: exponential penalty on the
        // ratio between the new and old interval, normalised per source tempo.
        const size_t nt = ss.intervals.size();
        for (int b = 0; b < ss.beatsPerBar; ++b) {
            const int prevBeat = (b + ss.beatsPerBar - 1) % ss.beatsPerBar;
            for (size_t from = 0; from < nt; ++from) {
                std::vector<double> p(nt);
                double sum = 0.0;
                for (size_t to = 0; to < nt; ++to) {
                    const double ratio = static_cast<double>(ss.intervals[to]) / ss.intervals[from];
                    double v = std::exp(-lambda * std::fabs(ratio - 1.0));
                    if (v <= std::numeric_limits<double>::epsilon()) v = 0.0;
                    p[to] = v;
                    sum += v;
                }
                for (size_t to = 0; to < nt; ++to) {
                    if (p[to] <= 0.0) continue;
                    rows[ss.firstStates[b][to]].push_back(
                        {static_cast<uint32_t>(ss.lastStates[prevBeat][from]), std::log(p[to] / sum)});
                }
            }
        }
        rowStart.assign(ss.numStates + 1, 0);
        for (int s = 0; s < ss.numStates; ++s) {
            rowStart[s + 1] = rowStart[s] + static_cast<uint32_t>(rows[s].size());
            for (const auto& e : rows[s]) {
                source.push_back(e.first);
                logProb.push_back(e.second);
            }
        }
    }
};

// Each state observes one of three densities: no beat, beat, downbeat.
struct Observation {
    std::vector<uint8_t> kind; // per state: 0 none, 1 beat, 2 downbeat
    int lambda;
    Observation(const StateSpace& ss, int observationLambda) : lambda(observationLambda) {
        const double border = 1.0 / lambda;
        kind.resize(ss.numStates);
        for (int s = 0; s < ss.numStates; ++s) {
            const double pos = ss.position[s];
            const double frac = pos - std::floor(pos);
            if (pos < border) kind[s] = 2;
            else if (frac < border) kind[s] = 1;
            else kind[s] = 0;
        }
    }
};

struct Frame {
    double beat, downbeat; // activations after the madmom combination step
};

struct Densities {
    std::vector<double> none, beat, downbeat; // log densities per frame
    explicit Densities(const std::vector<Frame>& obs, int lambda) {
        const size_t n = obs.size();
        none.resize(n);
        beat.resize(n);
        downbeat.resize(n);
        for (size_t t = 0; t < n; ++t) {
            none[t] = std::log(std::max((1.0 - (obs[t].beat + obs[t].downbeat)) / (lambda - 1.0), 1e-12));
            beat[t] = std::log(obs[t].beat);
            downbeat[t] = std::log(obs[t].downbeat);
        }
    }
    double at(size_t t, uint8_t kind) const { return kind == 0 ? none[t] : kind == 1 ? beat[t] : downbeat[t]; }
};

// Viterbi with sqrt(T) checkpointing so memory stays at O(N sqrt(T)).
std::vector<uint32_t> viterbi(const StateSpace& ss, const Transitions& tm, const Observation& om,
                              const std::vector<Frame>& obs) {
    const int T = static_cast<int>(obs.size());
    const int N = ss.numStates;
    if (T == 0) return {};
    const Densities dens(obs, om.lambda);
    const double logInit = -std::log(static_cast<double>(N));

    auto step = [&](const std::vector<double>& prev, std::vector<double>& cur, int t, std::vector<uint32_t>* back) {
        for (int s = 0; s < N; ++s) {
            double best = kNegInf;
            uint32_t bestSrc = 0;
            for (uint32_t e = tm.rowStart[s]; e < tm.rowStart[s + 1]; ++e) {
                const double v = prev[tm.source[e]] + tm.logProb[e];
                if (v > best) {
                    best = v;
                    bestSrc = tm.source[e];
                }
            }
            cur[s] = best + dens.at(static_cast<size_t>(t), om.kind[s]);
            if (back) (*back)[static_cast<size_t>(s)] = bestSrc;
        }
    };

    const int K = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(T))));
    const int segments = (T + K - 1) / K;
    std::vector<std::vector<double>> checkpoints(segments);
    std::vector<double> prev(N, logInit), cur(N);
    checkpoints[0] = prev;
    for (int t = 0; t < T; ++t) {
        step(prev, cur, t, nullptr);
        std::swap(prev, cur);
        const int next = (t + 1) / K;
        if (next < segments && (t + 1) == next * K) checkpoints[next] = prev;
    }
    int state = static_cast<int>(std::max_element(prev.begin(), prev.end()) - prev.begin());
    if (std::isinf(prev[state])) return {};

    std::vector<uint32_t> path(T);
    for (int seg = segments - 1; seg >= 0; --seg) {
        const int t0 = seg * K;
        const int t1 = std::min(T, (seg + 1) * K);
        std::vector<std::vector<uint32_t>> back(t1 - t0, std::vector<uint32_t>(N));
        std::vector<double> p = checkpoints[seg], c(N);
        for (int t = t0; t < t1; ++t) {
            step(p, c, t, &back[t - t0]);
            std::swap(p, c);
        }
        for (int t = t1 - 1; t >= t0; --t) {
            path[t] = static_cast<uint32_t>(state);
            state = static_cast<int>(back[t - t0][static_cast<size_t>(state)]);
        }
    }
    return path;
}

} // namespace

DbnConfig dbnConfigForTempo(double bpm, double tolerance, int beatsPerBar) {
    DbnConfig c;
    c.beatsPerBar = beatsPerBar;
    c.minBpm = bpm * (1.0 - tolerance);
    c.maxBpm = bpm * (1.0 + tolerance);
    return c;
}

BeatEvents decodeDbn(const BeatActivations& act, const DbnConfig& config) {
    if (act.beat.size() != act.downbeat.size()) throw std::invalid_argument("decodeDbn: size mismatch");
    if (config.minBpm <= 0.0 || config.maxBpm <= config.minBpm || config.beatsPerBar < 1)
        throw std::invalid_argument("decodeDbn: invalid configuration");

    // Logits -> probabilities -> madmom's combined (beat-only, downbeat) pair.
    const double eps = 1e-5;
    std::vector<Frame> obs(act.beat.size());
    for (size_t t = 0; t < obs.size(); ++t) {
        double b = 1.0 / (1.0 + std::exp(-static_cast<double>(act.beat[t])));
        double d = 1.0 / (1.0 + std::exp(-static_cast<double>(act.downbeat[t])));
        b = b * (1.0 - eps) + eps / 2.0;
        d = d * (1.0 - eps) + eps / 2.0;
        obs[t] = {std::max(b - d, eps / 2.0), d};
    }

    // Trim leading and trailing frames with no activity, as madmom does.
    size_t first = obs.size(), last = 0;
    for (size_t t = 0; t < obs.size(); ++t) {
        if (obs[t].beat >= config.threshold || obs[t].downbeat >= config.threshold) {
            if (first == obs.size()) first = t;
            last = t + 1;
        }
    }
    BeatEvents out;
    if (first >= last) return out;
    std::vector<Frame> trimmed(obs.begin() + static_cast<long>(first), obs.begin() + static_cast<long>(last));

    const int minInterval = static_cast<int>(std::lround(60.0 * config.fps / config.maxBpm));
    const int maxInterval = static_cast<int>(std::lround(60.0 * config.fps / config.minBpm));
    if (minInterval < 2 || maxInterval < minInterval) throw std::invalid_argument("decodeDbn: tempo range too narrow");
    const StateSpace ss(config.beatsPerBar, minInterval, maxInterval);
    const Transitions tm(ss, config.transitionLambda);
    const Observation om(ss, config.observationLambda);
    std::vector<uint32_t> path = viterbi(ss, tm, om, trimmed);
    if (path.empty()) return out;

    // Beat regions are runs of frames whose state is "on the beat"; place each
    // beat at the frame with the strongest activation inside its run.
    const size_t T = path.size();
    size_t t = 0;
    while (t < T) {
        if (om.kind[path[t]] == 0) {
            ++t;
            continue;
        }
        size_t end = t;
        while (end < T && om.kind[path[end]] != 0) ++end;
        size_t best = t;
        double bestVal = kNegInf;
        for (size_t i = t; i < end; ++i) {
            const double v = std::max(trimmed[i].beat, trimmed[i].downbeat);
            if (v > bestVal) {
                bestVal = v;
                best = i;
            }
        }
        const double time = static_cast<double>(best + first) / config.fps;
        out.beats.push_back(time);
        if (static_cast<int>(std::floor(ss.position[path[best]])) == 0) out.downbeats.push_back(time);
        t = end;
    }
    return out;
}

} // namespace srt
