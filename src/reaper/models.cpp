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

const ModelSpec kSeparationModel{
    "bsroformer-4s-v1",
    "bs_roformer_4stem_rhythm_fp16.onnx",
    "https://huggingface.co/silverdaw/bs-roformer-rhythm-onnx/resolve/main/bs_roformer_4stem_rhythm_fp16.onnx",
    "4bac38e42c085b5d69964e2b7ff161318a1315122c10b025c718b20e94d85d6b",
    269457840ull,
    "stem separation (BS-RoFormer 4-stem, MIT)",
};

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

} // namespace srt::reaper
