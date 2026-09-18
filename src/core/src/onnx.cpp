#include "srt/onnx.h"

#include "srt/log.h"
#include "srt/smoke_model.h"

#include <onnxruntime_cxx_api.h>
#if defined(SRT_HAVE_DML)
#include <dml_provider_factory.h>
#endif
#if defined(SRT_HAVE_COREML)
#include <coreml_provider_factory.h>
#endif

#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace srt {

const char* toString(Provider p) {
    switch (p) {
    case Provider::DirectML: return "DirectML";
    case Provider::CoreML: return "CoreML";
    case Provider::Cuda: return "CUDA";
    case Provider::Cpu: return "CPU";
    }
    return "unknown";
}

Ort::Env& OnnxRuntime::env() {
    static Ort::Env instance(ORT_LOGGING_LEVEL_WARNING, "srt");
    return instance;
}

std::string OnnxRuntime::version() { return Ort::GetVersionString(); }

std::vector<Provider> OnnxRuntime::availableProviders() {
    std::vector<Provider> out;
    std::vector<std::string> names;
    try {
        names = Ort::GetAvailableProviders();
    } catch (const Ort::Exception& ex) {
        logWarning(std::string("ONNX Runtime: cannot list providers: ") + ex.what());
    }
    auto has = [&](const char* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
#if defined(SRT_HAVE_DML)
    if (has("DmlExecutionProvider")) out.push_back(Provider::DirectML);
#endif
#if defined(SRT_HAVE_COREML)
    if (has("CoreMLExecutionProvider")) out.push_back(Provider::CoreML);
#endif
    if (has("CUDAExecutionProvider")) out.push_back(Provider::Cuda);
    out.push_back(Provider::Cpu);
    return out;
}

namespace {

void appendProvider(Ort::SessionOptions& opts, Provider p) {
    switch (p) {
    case Provider::DirectML: {
#if defined(SRT_HAVE_DML)
        // DirectML requires sequential execution and no memory pattern optimisation.
        opts.DisableMemPattern();
        opts.SetExecutionMode(ORT_SEQUENTIAL);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(opts, 0));
#else
        throw std::runtime_error("DirectML not compiled in");
#endif
        break;
    }
    case Provider::CoreML: {
#if defined(SRT_HAVE_COREML)
        std::unordered_map<std::string, std::string> o{
            {kCoremlProviderOption_MLComputeUnits, "MLComputeUnitsAll"},
            {kCoremlProviderOption_ModelFormat, "MLProgram"},
        };
        opts.AppendExecutionProvider("CoreML", o);
#else
        throw std::runtime_error("CoreML not compiled in");
#endif
        break;
    }
    case Provider::Cuda: {
        OrtCUDAProviderOptions cuda{};
        opts.AppendExecutionProvider_CUDA(cuda);
        break;
    }
    case Provider::Cpu: break;
    }
}

} // namespace

namespace {

template <typename MakeSession>
OnnxSession createWithFallback(const std::vector<Provider>& preference, const std::string& what,
                               MakeSession makeSession) {
    std::vector<Provider> order = preference;
    if (std::find(order.begin(), order.end(), Provider::Cpu) == order.end()) order.push_back(Provider::Cpu);

    std::string lastError;
    for (Provider p : order) {
        try {
            Ort::SessionOptions opts;
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            appendProvider(opts, p);
            OnnxSession out;
            out.session = makeSession(opts);
            out.provider = p;
            return out;
        } catch (const std::exception& ex) {
            lastError = ex.what();
            if (p != Provider::Cpu)
                logWarning(std::string("ONNX Runtime: ") + toString(p) + " unavailable, trying next: " + ex.what());
        }
    }
    throw std::runtime_error("cannot load model " + what + ": " + lastError);
}

} // namespace

OnnxSession OnnxRuntime::createSession(const std::filesystem::path& model,
                                       const std::vector<Provider>& preference) {
    return createWithFallback(preference, model.string(), [&](Ort::SessionOptions& opts) {
        return std::make_unique<Ort::Session>(env(), model.c_str(), opts);
    });
}

OnnxSession OnnxRuntime::createSession(const void* data, size_t size, const std::vector<Provider>& preference) {
    return createWithFallback(preference, "<memory>", [&](Ort::SessionOptions& opts) {
        return std::make_unique<Ort::Session>(env(), data, size, opts);
    });
}

namespace {

std::string runSmokeOn(OnnxSession s) {
    Ort::AllocatorWithDefaultOptions alloc;
    Ort::AllocatedStringPtr inName = s.session->GetInputNameAllocated(0, alloc);
    Ort::AllocatedStringPtr outName = s.session->GetOutputNameAllocated(0, alloc);
    const char* inNames[] = {inName.get()};
    const char* outNames[] = {outName.get()};

    std::vector<float> input{1.0f, 2.0f, 3.0f, 4.0f};
    const int64_t shape[] = {1, 4};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inTensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape, 2);

    auto outputs = s.session->Run(Ort::RunOptions{nullptr}, inNames, &inTensor, 1, outNames, 1);
    const float* out = outputs[0].GetTensorData<float>();
    const float expected[] = {3.0f, 5.0f, 7.0f, 9.0f};
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(out[i] - expected[i]) > 1e-5f) {
            std::ostringstream err;
            err << "smoke test mismatch at " << i << ": got " << out[i] << ", expected " << expected[i];
            throw std::runtime_error(err.str());
        }
    }
    std::ostringstream ok;
    ok << "ONNX Runtime " << OnnxRuntime::version() << " on " << toString(s.provider)
       << ": smoke model OK (2*x+1 over [1,2,3,4] = [3,5,7,9])";
    return ok.str();
}

} // namespace

std::string runSmokeTest(const std::filesystem::path& model, const std::vector<Provider>& preference) {
    return runSmokeOn(OnnxRuntime::createSession(model, preference));
}

std::string runEmbeddedSmokeTest(const std::vector<Provider>& preference) {
    return runSmokeOn(OnnxRuntime::createSession(kSrtSmokeModel, kSrtSmokeModel_size, preference));
}

} // namespace srt
