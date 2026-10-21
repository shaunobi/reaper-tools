// "SRT: Separate stems". See docs/SPEC.md section 4. The audio is read and
// the file names chosen on the main thread; separation and WAV writing run
// on a worker; the folder of tracks is built on the main thread afterwards.

#include "actions.h"
#include "crash_guard.h"
#include "host.h"
#include "item_audio.h"
#include "models.h"

#include "srt/demucs.h"
#include "srt/wav.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace srt::reaper {

namespace {

std::string sanitizeFileName(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "stem" : s;
}

std::filesystem::path uniquePath(const std::filesystem::path& dir, const std::string& base, const std::string& ext) {
    std::filesystem::path p = dir / (base + ext);
    for (int n = 2; std::filesystem::exists(p); ++n) p = dir / (base + " (" + std::to_string(n) + ")" + ext);
    return p;
}

std::filesystem::path projectMediaDir(ReaProject* proj) {
    char buf[4096] = {0};
    GetProjectPathEx(proj, buf, sizeof(buf));
    return buf;
}

struct SeparateJob : srt::Job {
    ReaProject* proj = nullptr;
    MediaItem* item = nullptr;
    MediaTrack* track = nullptr;
    std::string label;
    double itemPosition = 0.0;
    double itemLength = 0.0;
    double playrate = 1.0;
    std::vector<std::filesystem::path> modelPaths; // one per stem
    srt::AudioBuffer audio;
    std::vector<std::filesystem::path> outputs; // one per stem, chosen up front
    std::vector<std::string> stemNames;
    srt::Provider provider = srt::Provider::Cpu;
    double seconds = 0.0;

    std::string name() const override { return "Separate stems: " + label; }
    void run(srt::Progress& p) override {
        const auto t0 = std::chrono::steady_clock::now();
        p.set(0.01, "loading model");
        std::vector<srt::AudioBuffer> stems;
        std::string fault;
        const bool ok = runGuarded(
            [&] {
                srt::DemucsConfig cfg;
                cfg.modelPaths = modelPaths;
                srt::DemucsSeparator sep(cfg);
                provider = sep.provider();
                stemNames = sep.config().stemNames;
                stems = sep.separate(audio, &p);
            },
            fault);
        if (!ok) throw std::runtime_error(fault);
        if (stems.size() != outputs.size()) {
            // Stem count comes from the model; make sure every stem has a file name.
            const std::filesystem::path dir = outputs.front().parent_path();
            const std::string base = outputs.front().stem().string();
            outputs.clear();
            for (size_t s = 0; s < stems.size(); ++s)
                outputs.push_back(uniquePath(dir, base.substr(0, base.rfind(" - ")) + " - " + stemNames[s], ".wav"));
        }
        for (size_t s = 0; s < stems.size(); ++s) {
            if (p.cancelled()) throw srt::JobCancelled{};
            p.set(0.92 + 0.08 * static_cast<double>(s) / static_cast<double>(stems.size()), "writing " + stemNames[s]);
            srt::writeWav(outputs[s], stems[s], srt::WavFormat::Float32);
        }
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        p.set(1.0, "done");
    }
};

void onSeparateDone(const srt::JobStatus& st, srt::Job& base) {
    auto& job = static_cast<SeparateJob&>(base);
    if (st.state == srt::JobState::Cancelled) {
        for (const auto& f : job.outputs) {
            std::error_code ec;
            std::filesystem::remove(f, ec);
        }
        console("cancelled \"" + job.label + "\"");
        return;
    }
    if (st.state != srt::JobState::Succeeded) {
        console("failed on \"" + job.label + "\": " + st.error);
        return;
    }
    if (!ValidatePtr2(job.proj, job.item, "MediaItem*") || !ValidatePtr2(job.proj, job.track, "MediaTrack*")) {
        console("\"" + job.label + "\" is gone; stems were written but not inserted:");
        for (const auto& f : job.outputs) console("  " + f.string());
        return;
    }

    Undo_BeginBlock2(job.proj);
    // Folder directly below the source track. If the source closes a folder,
    // move that closing onto our last stem track so the structure stays valid.
    const int srcNumber = static_cast<int>(GetMediaTrackInfo_Value(job.track, "IP_TRACKNUMBER")); // 1-based
    int srcDepth = static_cast<int>(GetMediaTrackInfo_Value(job.track, "I_FOLDERDEPTH"));
    int closing = 0;
    if (srcDepth < 0) {
        closing = srcDepth;
        SetMediaTrackInfo_Value(job.track, "I_FOLDERDEPTH", 0);
    }
    const int folderIndex = srcNumber; // 0-based index right after the source
    InsertTrackAtIndex(folderIndex, true);
    MediaTrack* folder = GetTrack(job.proj, folderIndex);
    std::string folderName = job.label + " stems";
    GetSetMediaTrackInfo_String(folder, "P_NAME", folderName.data(), true);
    SetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH", 1);

    const size_t n = job.outputs.size();
    for (size_t s = 0; s < n; ++s) {
        const int idx = folderIndex + 1 + static_cast<int>(s);
        InsertTrackAtIndex(idx, true);
        MediaTrack* tr = GetTrack(job.proj, idx);
        std::string trackName = job.stemNames[s];
        GetSetMediaTrackInfo_String(tr, "P_NAME", trackName.data(), true);
        SetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH", s + 1 == n ? -1 + closing : 0);

        MediaItem* item = AddMediaItemToTrack(tr);
        SetMediaItemInfo_Value(item, "D_POSITION", job.itemPosition);
        SetMediaItemInfo_Value(item, "D_LENGTH", job.itemLength);
        MediaItem_Take* take = AddTakeToMediaItem(item);
        PCM_source* src = PCM_Source_CreateFromFile(job.outputs[s].string().c_str());
        if (src) SetMediaItemTake_Source(take, src);
        SetMediaItemTakeInfo_Value(take, "D_PLAYRATE", job.playrate);
        std::string takeName = job.label + " - " + job.stemNames[s];
        GetSetMediaItemTakeInfo_String(take, "P_NAME", takeName.data(), true);
        UpdateItemInProject(item);
    }
    Undo_EndBlock2(job.proj, "SRT: Separate stems", -1);
    TrackList_AdjustWindows(false);
    UpdateArrange();

    char line[512];
    std::snprintf(line, sizeof(line), "separated \"%s\" into %zu stems in %.1f s (Demucs v4 fine-tuned, %s)",
                  job.label.c_str(), n, job.seconds, srt::toString(job.provider));
    console(line);
}

void separateWithModels(const std::vector<std::filesystem::path>& models) {
    const int count = CountSelectedMediaItems(nullptr);
    std::vector<MediaItem*> items;
    for (int i = 0; i < count; ++i) items.push_back(GetSelectedMediaItem(nullptr, i));

    for (MediaItem* item : items) {
        MediaItem_Take* take = GetActiveTake(item);
        if (!take) {
            console("skipped an item with no active take");
            continue;
        }
        const char* nm = GetTakeName(take);
        const std::string label = nm && *nm ? nm : "(unnamed take)";
        SourceInfo info = describeSource(take);
        if (!info.isAudio) {
            console("skipped \"" + label + "\": not an audio take (" + info.type + ")");
            continue;
        }
        auto job = std::make_shared<SeparateJob>();
        job->proj = GetItemProjectContext(item);
        job->item = item;
        job->track = GetMediaItemTrack(item);
        job->label = label;
        job->itemPosition = GetMediaItemInfo_Value(item, "D_POSITION");
        job->itemLength = GetMediaItemInfo_Value(item, "D_LENGTH");
        job->playrate = GetMediaItemTakeInfo_Value(take, "D_PLAYRATE");
        if (job->playrate <= 0.0) job->playrate = 1.0;
        job->modelPaths = models;
        const double srcStart = GetMediaItemTakeInfo_Value(take, "D_STARTOFFS");
        double srcLength = job->itemLength * job->playrate;
        if (srcStart + srcLength > info.lengthSeconds) srcLength = info.lengthSeconds - srcStart;
        try {
            job->audio = readSourceAudio(take, srcStart, srcLength);
        } catch (const std::exception& ex) {
            console("skipped \"" + label + "\": " + ex.what());
            continue;
        }
        const std::filesystem::path dir = projectMediaDir(job->proj);
        const std::string base = sanitizeFileName(label);
        for (const char* stem : {"drums", "bass", "other", "vocals"})
            job->outputs.push_back(uniquePath(dir, base + " - " + stem, ".wav"));
        job->stemNames = {"drums", "bass", "other", "vocals"};

        char line[256];
        std::snprintf(line, sizeof(line), "separating \"%s\" (%.1f s of audio at %.0f Hz)", label.c_str(),
                      job->audio.durationSeconds(), job->audio.sampleRate);
        console(line);
        jobs().submit(job, &onSeparateDone);
    }
}

} // namespace

void actionSeparateStems() {
    if (CountSelectedMediaItems(nullptr) == 0) {
        console("select at least one item first");
        return;
    }
    std::vector<const ModelSpec*> specs;
    for (size_t i = 0; i < kDemucsModelCount; ++i) specs.push_back(&kDemucsModels[i]);
    ensureModels(specs, &separateWithModels);
}

} // namespace srt::reaper
