#pragma once
// Where model files live and how to find them. Downloading is a later
// milestone; for now a missing model is reported with instructions.

#include <filesystem>
#include <string>

namespace srt::reaper {

struct ModelSpec {
    const char* id; // folder name under <resource>/SRT/models
    const char* fileName;
    const char* url;
    const char* sha256;
    const char* purpose;
};

extern const ModelSpec kBeatThisModel;

std::filesystem::path modelsRoot();
std::filesystem::path modelPath(const ModelSpec& spec);

// Returns the path if the model is present, otherwise prints instructions to
// the console and returns an empty path.
std::filesystem::path requireModel(const ModelSpec& spec);

} // namespace srt::reaper
