#include "models.h"

#include "host.h"

namespace srt::reaper {

const ModelSpec kBeatThisModel{
    "beatthis-final0-v1",
    "beat-this-final0.onnx",
    "https://huggingface.co/aaatmy/beat-this-onnx/resolve/main/beat-this-final0.onnx",
    "e5e37b7d1802895e42559c5a1ced7619b1601d257a5c8e167c9d45de37c1b078",
    "beat and downbeat tracking",
};

std::filesystem::path modelsRoot() { return std::filesystem::path(GetResourcePath()) / "SRT" / "models"; }

std::filesystem::path modelPath(const ModelSpec& spec) { return modelsRoot() / spec.id / spec.fileName; }

std::filesystem::path requireModel(const ModelSpec& spec) {
    std::filesystem::path p = modelPath(spec);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return p;
    console(std::string("the model for ") + spec.purpose + " is not installed");
    console("  download: " + std::string(spec.url));
    console("  save as:  " + p.string());
    console("  (automatic download is planned; for now copy the file there and run the action again)");
    return {};
}

} // namespace srt::reaper
