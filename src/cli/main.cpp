// srt_cli: command-line harness over srt_core. Everything the REAPER
// extension can do to audio is reachable from here so it can be tested
// without REAPER.

#include "srt/beat_tracker.h"
#include "srt/jobs.h"
#include "srt/log.h"
#include "srt/mel.h"
#include "srt/onnx.h"
#include "srt/onset.h"
#include "srt/resample.h"
#include "srt/smoke_model.h"
#include "srt/version.h"
#include "srt/wav.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int usage() {
    std::puts("srt_cli " SRT_VERSION_STRING "\n"
              "usage:\n"
              "  srt_cli version                 print version and ONNX Runtime version\n"
              "  srt_cli providers               list usable execution providers, best first\n"
              "  srt_cli smoke [model.onnx]      run the smoke model (embedded copy if no path)\n"
              "  srt_cli wavinfo <file.wav>      print channel count, rate and length\n"
              "  srt_cli beats <file.wav> --model <beat-this.onnx> [--provider cpu|directml|coreml]\n"
              "                                  [--no-refine] [--csv]\n"
              "                                  detect beats and downbeats; prints tempo and event times\n"
              "  srt_cli mel <file.wav> <out.f32>  write the log-mel spectrogram (frames x 128 float32)\n");
    return 2;
}

srt::Provider parseProvider(const std::string& s) {
    if (s == "cpu") return srt::Provider::Cpu;
    if (s == "directml" || s == "dml") return srt::Provider::DirectML;
    if (s == "coreml") return srt::Provider::CoreML;
    if (s == "cuda") return srt::Provider::Cuda;
    throw std::runtime_error("unknown provider: " + s);
}

} // namespace

int main(int argc, char** argv) {
    srt::setLogSink([](srt::LogLevel level, const std::string& m) {
        const char* tag = level == srt::LogLevel::Error ? "error" : level == srt::LogLevel::Warning ? "warning" : "info";
        std::fprintf(stderr, "[%s] %s\n", tag, m.c_str());
    });

    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    try {
        if (cmd == "version") {
            std::printf("srt_cli %s, ONNX Runtime %s\n", SRT_VERSION_STRING, srt::OnnxRuntime::version().c_str());
            return 0;
        }
        if (cmd == "providers") {
            for (srt::Provider p : srt::OnnxRuntime::availableProviders()) std::printf("%s\n", srt::toString(p));
            return 0;
        }
        if (cmd == "smoke") {
            std::vector<srt::Provider> pref = srt::OnnxRuntime::availableProviders();
            std::string modelPath;
            for (int i = 2; i < argc; ++i) {
                if (std::strcmp(argv[i], "--provider") == 0 && i + 1 < argc) pref = {parseProvider(argv[++i])};
                else modelPath = argv[i];
            }
            std::string report = modelPath.empty() ? srt::runEmbeddedSmokeTest(pref) : srt::runSmokeTest(modelPath, pref);
            std::printf("%s\n", report.c_str());
            return 0;
        }
        if (cmd == "beats") {
            if (argc < 3) return usage();
            std::string wavPath = argv[2], modelPath;
            std::vector<srt::Provider> pref = srt::OnnxRuntime::availableProviders();
            bool refine = true, csv = false;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--model" && i + 1 < argc) modelPath = argv[++i];
                else if (a == "--provider" && i + 1 < argc) pref = {parseProvider(argv[++i])};
                else if (a == "--no-refine") refine = false;
                else if (a == "--csv") csv = true;
                else return usage();
            }
            if (modelPath.empty()) {
                std::fprintf(stderr, "error: --model is required\n");
                return 2;
            }
            srt::AudioBuffer audio = srt::readWav(wavPath);
            srt::BeatTrackerConfig cfg;
            cfg.modelPath = modelPath;
            cfg.providers = pref;
            srt::BeatTracker tracker(cfg);
            srt::BeatActivations act = tracker.activations(audio);
            srt::BeatEvents ev = srt::pickPeaksMinimal(act);
            std::vector<double> beats = ev.beats, downbeats = ev.downbeats;
            if (refine) {
                std::vector<float> mono = audio.mixToMono();
                beats = srt::refineOnsets(mono, audio.sampleRate, beats);
                downbeats = srt::refineOnsets(mono, audio.sampleRate, downbeats);
            }
            const double bpm = srt::estimateTempoBpm(ev.beats);
            if (csv) {
                std::printf("time,is_downbeat\n");
                size_t d = 0;
                for (size_t i = 0; i < beats.size(); ++i) {
                    bool isDown = false;
                    while (d < ev.downbeats.size() && ev.downbeats[d] < ev.beats[i] - 1e-9) ++d;
                    if (d < ev.downbeats.size() && std::fabs(ev.downbeats[d] - ev.beats[i]) < 1e-9) isDown = true;
                    std::printf("%.6f,%d\n", beats[i], isDown ? 1 : 0);
                }
                return 0;
            }
            std::printf("provider: %s\n", srt::toString(tracker.provider()));
            std::printf("tempo: %.2f BPM (fit over %zu beats)\n", bpm, ev.beats.size());
            std::printf("beats: %zu, downbeats: %zu%s\n", beats.size(), downbeats.size(),
                        refine ? " (onset-refined)" : "");
            for (size_t i = 0; i < downbeats.size(); ++i) {
                double next = i + 1 < downbeats.size() ? downbeats[i + 1] - downbeats[i] : 0.0;
                std::printf("  bar %3zu  %9.4f s%s\n", i + 1, downbeats[i],
                            next > 0.0 ? (std::string("  (bar length ") + std::to_string(next) + " s)").c_str() : "");
            }
            return 0;
        }
        if (cmd == "mel") {
            if (argc < 4) return usage();
            srt::AudioBuffer audio = srt::readWav(argv[2]);
            std::vector<float> mono = audio.mixToMono();
            srt::LogMelSpectrogram mel;
            mono = srt::resample(mono, audio.sampleRate, mel.config().sampleRate);
            std::vector<float> spect = mel.compute(mono.data(), mono.size());
            std::ofstream f(argv[3], std::ios::binary);
            f.write(reinterpret_cast<const char*>(spect.data()), static_cast<std::streamsize>(spect.size() * 4));
            std::printf("%zu frames x %d mels written to %s\n", mel.numFrames(mono.size()), mel.config().nMels, argv[3]);
            return 0;
        }
        if (cmd == "wavinfo") {
            if (argc < 3) return usage();
            srt::AudioBuffer a = srt::readWav(argv[2]);
            std::printf("%s: %zu channels, %.0f Hz, %zu frames, %.3f s\n", argv[2], a.numChannels(), a.sampleRate,
                        a.numFrames(), a.durationSeconds());
            return 0;
        }
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
