#include "models.h"

#include "host.h"
#include "srt/download.h"

#include <cstdio>
#include <memory>

namespace srt::reaper {

const ModelSpec kBeatThisModel{
    "beatthis-final0-v1",
    "beat-this-final0.onnx",
    "https://huggingface.co/aaatmy/beat-this-onnx/resolve/main/beat-this-final0.onnx",
    "e5e37b7d1802895e42559c5a1ced7619b1601d257a5c8e167c9d45de37c1b078",
    82098846ull,
    "beat and downbeat tracking (Beat This!, MIT)",
};

// Exported from the official demucs package by tools/export_demucs.py (MIT
// weights by Meta AI). Hosted as release assets of this repository.
#define SRT_DEMUCS_URL(file) "https://github.com/shaunobi/reaper-tools/releases/download/models-v1/" file
const ModelSpec kDemucsModels[kDemucsModelCount] = {
    {"htdemucs-ft-v1", "htdemucs_ft_drums.onnx", SRT_DEMUCS_URL("htdemucs_ft_drums.onnx"),
     "2a8ba51dc7aecc01703f080497b579534fe60f0cb54b3adb44e3892d68091b72", 174368116ull,
     "stem separation (Demucs v4 fine-tuned, drums model, MIT)"},
    {"htdemucs-ft-v1", "htdemucs_ft_bass.onnx", SRT_DEMUCS_URL("htdemucs_ft_bass.onnx"),
     "0eb1ab8a8402bf40c9a3ea78836ea76f5af972b9860d5fa12790abee55533876", 174368116ull,
     "stem separation (Demucs v4 fine-tuned, bass model, MIT)"},
    {"htdemucs-ft-v1", "htdemucs_ft_other.onnx", SRT_DEMUCS_URL("htdemucs_ft_other.onnx"),
     "96e366848d855a5bba4fc725c90cd5f912fee75f05489f0571f8029f59283371", 174368116ull,
     "stem separation (Demucs v4 fine-tuned, other model, MIT)"},
    {"htdemucs-ft-v1", "htdemucs_ft_vocals.onnx", SRT_DEMUCS_URL("htdemucs_ft_vocals.onnx"),
     "23fca3d2ac4cce8a52bdca1586484423ebee8bf5d584788b21c3f85c383b0581", 174368116ull,
     "stem separation (Demucs v4 fine-tuned, vocals model, MIT)"},
};
#undef SRT_DEMUCS_URL

std::filesystem::path modelsRoot() { return std::filesystem::path(GetResourcePath()) / "SRT" / "models"; }

std::filesystem::path modelPath(const ModelSpec& spec) { return modelsRoot() / spec.id / spec.fileName; }

namespace {

struct DownloadJob : srt::Job {
    const ModelSpec* spec = nullptr;
    std::filesystem::path dest;
    std::function<void(const std::filesystem::path&)> then;
    std::string name() const override { return std::string("Downloading model: ") + spec->id; }
    void run(srt::Progress& p) override { srt::downloadFile(spec->url, dest, &p, spec->sha256); }
};

void onDownloadDone(const srt::JobStatus& st, srt::Job& base) {
    auto& job = static_cast<DownloadJob&>(base);
    if (st.state == srt::JobState::Succeeded) {
        console(std::string("model installed: ") + job.dest.string());
        if (job.then) job.then(job.dest);
    } else if (st.state == srt::JobState::Cancelled) {
        console("model download cancelled");
    } else {
        console("model download failed: " + st.error);
        console("  you can download it by hand from " + std::string(job.spec->url));
        console("  and save it as " + job.dest.string());
    }
}

} // namespace

void ensureModel(const ModelSpec& spec, std::function<void(const std::filesystem::path&)> then) {
    const std::filesystem::path p = modelPath(spec);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        then(p);
        return;
    }
    if (!srt::downloadSupported()) {
        console(std::string("the model for ") + spec.purpose + " is not installed and this build cannot download");
        console("  download: " + std::string(spec.url));
        console("  save as:  " + p.string());
        return;
    }
    char msg[1024];
    std::snprintf(msg, sizeof(msg),
                  "SRT needs a model for %s.\n\nDownload %.0f MB from\n%s\n\nto\n%s\n\nThis happens once. Continue?",
                  spec.purpose, spec.sizeBytes / 1e6, spec.url, p.string().c_str());
    if (ShowMessageBox(msg, "SRT: download model", 4) != 6) {
        console(std::string("model download declined; the action needs ") + spec.fileName);
        return;
    }
    auto job = std::make_shared<DownloadJob>();
    job->spec = &spec;
    job->dest = p;
    job->then = std::move(then);
    console(std::string("downloading ") + spec.id + " (" + std::to_string(spec.sizeBytes / 1000000) + " MB)");
    jobs().submit(job, &onDownloadDone);
}

namespace {

// Downloads the missing files one after another (each its own job with
// progress), then hands all paths to `then`.
void downloadMissing(std::vector<const ModelSpec*> missing, size_t index,
                     std::function<void(const std::vector<std::filesystem::path>&)> then,
                     std::vector<std::filesystem::path> all) {
    if (index >= missing.size()) {
        then(all);
        return;
    }
    const ModelSpec* spec = missing[index];
    auto job = std::make_shared<DownloadJob>();
    job->spec = spec;
    job->dest = modelPath(*spec);
    job->then = [missing, index, then, all](const std::filesystem::path&) {
        downloadMissing(missing, index + 1, then, all);
    };
    console(std::string("downloading ") + spec->fileName + " (" + std::to_string(spec->sizeBytes / 1000000) + " MB, " +
            std::to_string(index + 1) + " of " + std::to_string(missing.size()) + ")");
    jobs().submit(job, &onDownloadDone);
}

} // namespace

void ensureModels(std::vector<const ModelSpec*> specs,
                  std::function<void(const std::vector<std::filesystem::path>&)> then) {
    std::vector<std::filesystem::path> all;
    std::vector<const ModelSpec*> missing;
    uint64_t bytes = 0;
    for (const ModelSpec* spec : specs) {
        const std::filesystem::path p = modelPath(*spec);
        all.push_back(p);
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            missing.push_back(spec);
            bytes += spec->sizeBytes;
        }
    }
    if (missing.empty()) {
        then(all);
        return;
    }
    if (!srt::downloadSupported()) {
        console(std::string("the models for ") + specs.front()->purpose + " are not installed and this build cannot download");
        for (const ModelSpec* spec : missing) {
            console("  download: " + std::string(spec->url));
            console("  save as:  " + modelPath(*spec).string());
        }
        return;
    }
    std::string files;
    for (const ModelSpec* spec : missing) files += std::string("  ") + spec->fileName + "\n";
    char msg[2048];
    std::snprintf(msg, sizeof(msg),
                  "SRT needs %zu model file(s) for %s.\n\nDownload %.0f MB in total from\n%s\n\n%s\ninto\n%s\n\nThis happens once. Continue?",
                  missing.size(), specs.front()->purpose, bytes / 1e6,
                  std::filesystem::path(missing.front()->url).parent_path().generic_string().c_str(), files.c_str(),
                  modelPath(*missing.front()).parent_path().string().c_str());
    if (ShowMessageBox(msg, "SRT: download models", 4) != 6) {
        console("model download declined; the action needs " + std::to_string(missing.size()) + " more file(s)");
        return;
    }
    downloadMissing(missing, 0, std::move(then), all);
}

} // namespace srt::reaper
