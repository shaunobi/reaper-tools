#pragma once
// Model files: where they live, and an ask-then-download flow that runs the
// action once the model is in place.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace srt::reaper {

struct ModelSpec {
    const char* id; // folder name under <resource>/SRT/models
    const char* fileName;
    const char* url;
    const char* sha256;
    uint64_t sizeBytes;
    const char* purpose;
};

extern const ModelSpec kBeatThisModel;
extern const ModelSpec kSeparationModel;

std::filesystem::path modelsRoot();
std::filesystem::path modelPath(const ModelSpec& spec);

// Calls `then(path)` immediately if the model is installed. Otherwise asks
// the user, downloads it as a background job with progress, verifies the
// checksum, and calls `then` on the main thread when done. On refusal or
// failure `then` is never called and the console explains why.
void ensureModel(const ModelSpec& spec, std::function<void(const std::filesystem::path&)> then);

} // namespace srt::reaper
