#include "srt/download.h"

#include "srt/sha256.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace srt {

// Implemented by the platform back end: streams the body of `url` into
// `out`, calling onBytes(received, total) as it goes (total may be 0).
void downloadToStream(const std::string& url, std::ofstream& out,
                      const std::function<void(uint64_t, uint64_t)>& onBytes, const std::function<bool()>& cancelled);

void downloadFile(const std::string& url, const std::filesystem::path& dest, Progress* progress,
                  const std::string& expectedSha256) {
    if (!downloadSupported()) throw std::runtime_error("downloads are not supported in this build");
    std::filesystem::create_directories(dest.parent_path());
    const std::filesystem::path part = dest.string() + ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create " + part.string());
        auto onBytes = [&](uint64_t got, uint64_t total) {
            if (!progress) return;
            char msg[96];
            if (total > 0)
                std::snprintf(msg, sizeof(msg), "downloading %.0f of %.0f MB", got / 1e6, total / 1e6);
            else
                std::snprintf(msg, sizeof(msg), "downloading %.0f MB", got / 1e6);
            progress->set(total > 0 ? 0.9 * static_cast<double>(got) / static_cast<double>(total) : 0.0, msg);
        };
        auto cancelled = [&]() { return progress && progress->cancelled(); };
        try {
            downloadToStream(url, out, onBytes, cancelled);
        } catch (...) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(part, ec);
            throw;
        }
    }
    if (!expectedSha256.empty()) {
        if (progress) progress->set(0.92, "verifying checksum");
        const std::string got = sha256File(part);
        if (got != expectedSha256) {
            std::error_code ec;
            std::filesystem::remove(part, ec);
            throw std::runtime_error("checksum mismatch: expected " + expectedSha256 + ", got " + got);
        }
    }
    std::error_code ec;
    std::filesystem::remove(dest, ec);
    std::filesystem::rename(part, dest, ec);
    if (ec) throw std::runtime_error("cannot move " + part.string() + " into place: " + ec.message());
    if (progress) progress->set(1.0, "done");
}

} // namespace srt
