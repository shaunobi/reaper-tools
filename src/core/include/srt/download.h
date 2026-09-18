#pragma once
// HTTPS download with progress, cancellation and SHA-256 verification.
// Platform back ends: WinHTTP on Windows, libcurl elsewhere.

#include "srt/jobs.h"

#include <filesystem>
#include <string>

namespace srt {

// Downloads url to dest (writing dest with a ".part" suffix first), verifies
// the checksum when expectedSha256 is not empty, then renames into place.
// Throws std::runtime_error on failure and JobCancelled on cancel.
void downloadFile(const std::string& url, const std::filesystem::path& dest, Progress* progress = nullptr,
                  const std::string& expectedSha256 = std::string());

// True when this build can download at all.
bool downloadSupported();

} // namespace srt
