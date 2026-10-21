#pragma once
// Thin wrapper around ONNX Runtime: one shared environment, execution
// provider selection with fallback, and a smoke test used by "SRT: About"
// and the test suite to prove the runtime works on this machine.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Ort {
class Env;
class Session;
} // namespace Ort

namespace srt {

enum class Provider { DirectML, CoreML, Cuda, Cpu };

const char* toString(Provider p);

struct OnnxSession {
    std::unique_ptr<Ort::Session> session;
    Provider provider = Provider::Cpu;
};

class OnnxRuntime {
public:
    static Ort::Env& env();
    static std::string version();

    // Providers this build can use, best first. Cpu is always last.
    static std::vector<Provider> availableProviders();

    // Creates a session trying each provider in `preference` in order, then
    // Cpu. Throws std::runtime_error if the model cannot be loaded at all.
    static OnnxSession createSession(const std::filesystem::path& model,
                                     const std::vector<Provider>& preference = availableProviders());
    // Same, from a model held in memory.
    static OnnxSession createSession(const void* data, size_t size,
                                     const std::vector<Provider>& preference = availableProviders());
};

// Runs tests/data/smoke.onnx-style model: input float[1,4] -> output = 2*x + 1.
// Returns a human-readable report line and throws on mismatch.
std::string runSmokeTest(const std::filesystem::path& model,
                         const std::vector<Provider>& preference = OnnxRuntime::availableProviders());
// Same test using the copy of the smoke model compiled into srt_core.
std::string runEmbeddedSmokeTest(const std::vector<Provider>& preference = OnnxRuntime::availableProviders());

} // namespace srt
