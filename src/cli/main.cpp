// srt_cli: command-line harness over srt_core. Everything the REAPER
// extension can do to audio is reachable from here so it can be tested
// without REAPER.

#include "srt/jobs.h"
#include "srt/log.h"
#include "srt/onnx.h"
#include "srt/smoke_model.h"
#include "srt/version.h"
#include "srt/wav.h"

#include <cstdio>
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
              "  srt_cli wavinfo <file.wav>      print channel count, rate and length\n");
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
