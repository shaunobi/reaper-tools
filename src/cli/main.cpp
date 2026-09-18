// srt_cli: command-line harness over srt_core. Everything the REAPER
// extension can do to audio is reachable from here so it can be tested
// without REAPER.

#include "srt/beat_detect.h"
#include "srt/beat_tracker.h"
#include "srt/jobs.h"
#include "srt/log.h"
#include "srt/mel.h"
#include "srt/onnx.h"
#include "srt/onset.h"
#include "srt/resample.h"
#include "srt/separator.h"
#include "srt/smoke_model.h"
#include "srt/version.h"
#include "srt/wav.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
              "                                  [--tempo <bpm>] [--minimal] [--no-refine] [--csv]\n"
              "                                  detect beats and downbeats; --tempo constrains the decoder\n"
              "                                  to +/-15% of the given tempo, --minimal uses plain peak picking\n"
              "  srt_cli mel <file.wav> <out.f32>  write the log-mel spectrogram (frames x 128 float32)\n"
              "  srt_cli separate <file.wav> --model <bs-roformer.onnx> [--out-dir <dir>] [--provider ...]\n"
              "                                  write one 32-bit float WAV per stem next to the input\n");
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
            srt::BeatDetectOptions opt;
            bool csv = false;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--model" && i + 1 < argc) modelPath = argv[++i];
                else if (a == "--provider" && i + 1 < argc) pref = {parseProvider(argv[++i])};
                else if (a == "--tempo" && i + 1 < argc) opt.tempoHintBpm = std::atof(argv[++i]);
                else if (a == "--minimal") opt.useDbn = false;
                else if (a == "--no-refine") opt.refineOnsets = false;
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
            srt::BeatDetectResult r = srt::detectBeats(tracker, audio, opt);
            if (csv) {
                std::printf("time,is_downbeat\n");
                size_t d = 0;
                for (size_t i = 0; i < r.events.beats.size(); ++i) {
                    bool isDown = false;
                    while (d < r.modelEvents.downbeats.size() && r.modelEvents.downbeats[d] < r.modelEvents.beats[i] - 1e-9) ++d;
                    if (d < r.modelEvents.downbeats.size() && std::fabs(r.modelEvents.downbeats[d] - r.modelEvents.beats[i]) < 1e-9)
                        isDown = true;
                    std::printf("%.6f,%d\n", r.events.beats[i], isDown ? 1 : 0);
                }
                return 0;
            }
            std::printf("provider: %s, decoder: %s%s\n", srt::toString(r.provider), opt.useDbn ? "DBN" : "peak picking",
                        opt.tempoHintBpm > 0.0 ? (" with tempo prior " + std::to_string(opt.tempoHintBpm) + " BPM").c_str() : "");
            std::printf("tempo: %.2f BPM (fit over %zu beats)\n", r.tempoBpm, r.modelEvents.beats.size());
            std::printf("confidence: %s\n", srt::toString(r.confidence));
            for (const auto& n : r.notes) std::printf("  note: %s\n", n.c_str());
            std::printf("beats: %zu, downbeats: %zu%s\n", r.events.beats.size(), r.events.downbeats.size(),
                        opt.refineOnsets ? " (onset-refined)" : "");
            const auto& downbeats = r.events.downbeats;
            for (size_t i = 0; i < downbeats.size(); ++i) {
                double next = i + 1 < downbeats.size() ? downbeats[i + 1] - downbeats[i] : 0.0;
                if (next > 0.0) std::printf("  bar %3zu  %9.4f s  (bar length %.4f s)\n", i + 1, downbeats[i], next);
                else std::printf("  bar %3zu  %9.4f s\n", i + 1, downbeats[i]);
            }
            return 0;
        }
        if (cmd == "separate") {
            if (argc < 3) return usage();
            std::string wavPath = argv[2], modelPath, outDir;
            std::vector<srt::Provider> pref = srt::OnnxRuntime::availableProviders();
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--model" && i + 1 < argc) modelPath = argv[++i];
                else if (a == "--out-dir" && i + 1 < argc) outDir = argv[++i];
                else if (a == "--provider" && i + 1 < argc) pref = {parseProvider(argv[++i])};
                else return usage();
            }
            if (modelPath.empty()) {
                std::fprintf(stderr, "error: --model is required\n");
                return 2;
            }
            srt::AudioBuffer audio = srt::readWav(wavPath);
            srt::SeparatorConfig cfg;
            cfg.modelPath = modelPath;
            cfg.providers = pref;
            srt::StemSeparator sep(cfg);
            std::printf("provider: %s, %zu stems, chunk %.1f s\n", srt::toString(sep.provider()), sep.numStems(),
                        sep.chunkSamples() / static_cast<double>(cfg.sampleRate));
            srt::Progress progress;
            std::vector<srt::AudioBuffer> stems = sep.separate(audio, &progress);
            std::filesystem::path in(wavPath);
            std::filesystem::path dir = outDir.empty() ? in.parent_path() : std::filesystem::path(outDir);
            std::filesystem::create_directories(dir.empty() ? "." : dir);
            for (size_t s = 0; s < stems.size(); ++s) {
                std::filesystem::path out = dir / (in.stem().string() + " - " + sep.config().stemNames[s] + ".wav");
                srt::writeWav(out, stems[s]);
                std::printf("wrote %s\n", out.string().c_str());
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
