#pragma once
// SHA-256 for verifying downloaded model files.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace srt {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    // Lowercase hex digest. The object must not be used afterwards.
    std::string finish();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buffer_[64];
    size_t bufferLen_ = 0;
    uint64_t total_ = 0;
};

std::string sha256Hex(const void* data, size_t size);
std::string sha256File(const std::filesystem::path& path);

} // namespace srt
