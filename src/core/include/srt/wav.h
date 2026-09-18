#pragma once
// Minimal WAV reader and writer. Reads PCM 16/24/32-bit and IEEE float 32/64,
// including WAVE_FORMAT_EXTENSIBLE. Writes 32-bit float by default.

#include "srt/audio.h"

#include <filesystem>
#include <string>

namespace srt {

enum class WavFormat { Float32, Pcm16, Pcm24 };

// Throws std::runtime_error with a readable message on failure.
AudioBuffer readWav(const std::filesystem::path& path);
void writeWav(const std::filesystem::path& path, const AudioBuffer& audio,
              WavFormat format = WavFormat::Float32);

} // namespace srt
