#include "srt/wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace srt {
namespace {

uint16_t rd16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(std::ofstream& f, uint16_t v) {
    unsigned char b[2] = {static_cast<unsigned char>(v & 0xff), static_cast<unsigned char>(v >> 8)};
    f.write(reinterpret_cast<const char*>(b), 2);
}
void wr32(std::ofstream& f, uint32_t v) {
    unsigned char b[4] = {static_cast<unsigned char>(v & 0xff), static_cast<unsigned char>((v >> 8) & 0xff),
                          static_cast<unsigned char>((v >> 16) & 0xff), static_cast<unsigned char>(v >> 24)};
    f.write(reinterpret_cast<const char*>(b), 4);
}

std::string pathString(const std::filesystem::path& p) { return p.string(); }

} // namespace

AudioBuffer readWav(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + pathString(path));
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a WAV file: " + pathString(path));

    uint16_t formatTag = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const unsigned char* pcm = nullptr;
    size_t pcmBytes = 0;

    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const unsigned char* chunk = data.data() + pos;
        uint32_t size = rd32(chunk + 4);
        const unsigned char* body = chunk + 8;
        size_t avail = data.size() - (pos + 8);
        if (size > avail) size = static_cast<uint32_t>(avail);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            formatTag = rd16(body);
            channels = rd16(body + 2);
            sampleRate = rd32(body + 4);
            bitsPerSample = rd16(body + 14);
            if (formatTag == 0xFFFE && size >= 40) formatTag = rd16(body + 24); // extensible: sub-format
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            pcm = body;
            pcmBytes = size;
        }
        pos += 8 + size + (size & 1);
    }
    if (!pcm || channels == 0 || sampleRate == 0 || bitsPerSample == 0)
        throw std::runtime_error("WAV file is missing fmt or data chunk: " + pathString(path));

    const size_t bytesPerSample = bitsPerSample / 8;
    const size_t frames = pcmBytes / (bytesPerSample * channels);
    AudioBuffer out(channels, frames, static_cast<double>(sampleRate));

    const bool isFloat = (formatTag == 3);
    const bool isPcm = (formatTag == 1);
    if (!isFloat && !isPcm) throw std::runtime_error("unsupported WAV format tag in " + pathString(path));

    const unsigned char* p = pcm;
    for (size_t i = 0; i < frames; ++i) {
        for (size_t c = 0; c < channels; ++c) {
            float v = 0.0f;
            if (isFloat && bitsPerSample == 32) {
                uint32_t u = rd32(p);
                std::memcpy(&v, &u, 4);
            } else if (isFloat && bitsPerSample == 64) {
                uint64_t u = static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
                double d;
                std::memcpy(&d, &u, 8);
                v = static_cast<float>(d);
            } else if (isPcm && bitsPerSample == 16) {
                v = static_cast<int16_t>(rd16(p)) / 32768.0f;
            } else if (isPcm && bitsPerSample == 24) {
                int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
                v = static_cast<float>(s >> 8) / 8388608.0f;
            } else if (isPcm && bitsPerSample == 32) {
                v = static_cast<int32_t>(rd32(p)) / 2147483648.0f;
            } else if (isPcm && bitsPerSample == 8) {
                v = (static_cast<int>(p[0]) - 128) / 128.0f;
            } else {
                throw std::runtime_error("unsupported WAV bit depth in " + pathString(path));
            }
            out.channels[c][i] = v;
            p += bytesPerSample;
        }
    }
    return out;
}

void writeWav(const std::filesystem::path& path, const AudioBuffer& audio, WavFormat format) {
    if (audio.numChannels() == 0 || audio.sampleRate <= 0.0)
        throw std::runtime_error("writeWav: empty buffer or invalid sample rate");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot create " + pathString(path));

    const uint16_t channels = static_cast<uint16_t>(audio.numChannels());
    const uint32_t rate = static_cast<uint32_t>(std::lround(audio.sampleRate));
    const uint16_t bits = format == WavFormat::Float32 ? 32 : format == WavFormat::Pcm24 ? 24 : 16;
    const uint16_t tag = format == WavFormat::Float32 ? 3 : 1;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bits / 8);
    const uint32_t dataBytes = static_cast<uint32_t>(audio.numFrames() * blockAlign);
    const bool addFact = (tag == 3);
    const uint32_t riffSize = 4 + (8 + 16) + (addFact ? 12 : 0) + (8 + dataBytes);

    f.write("RIFF", 4);
    wr32(f, riffSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    wr32(f, 16);
    wr16(f, tag);
    wr16(f, channels);
    wr32(f, rate);
    wr32(f, rate * blockAlign);
    wr16(f, blockAlign);
    wr16(f, bits);
    if (addFact) {
        f.write("fact", 4);
        wr32(f, 4);
        wr32(f, static_cast<uint32_t>(audio.numFrames()));
    }
    f.write("data", 4);
    wr32(f, dataBytes);

    std::vector<unsigned char> buf(blockAlign);
    for (size_t i = 0; i < audio.numFrames(); ++i) {
        unsigned char* p = buf.data();
        for (uint16_t c = 0; c < channels; ++c) {
            float v = audio.channels[c][i];
            if (format == WavFormat::Float32) {
                uint32_t u;
                std::memcpy(&u, &v, 4);
                p[0] = u & 0xff; p[1] = (u >> 8) & 0xff; p[2] = (u >> 16) & 0xff; p[3] = u >> 24;
                p += 4;
            } else if (format == WavFormat::Pcm24) {
                float cl = std::clamp(v, -1.0f, 1.0f);
                int32_t s = static_cast<int32_t>(std::lround(cl * 8388607.0f));
                p[0] = s & 0xff; p[1] = (s >> 8) & 0xff; p[2] = (s >> 16) & 0xff;
                p += 3;
            } else {
                float cl = std::clamp(v, -1.0f, 1.0f);
                int16_t s = static_cast<int16_t>(std::lround(cl * 32767.0f));
                p[0] = s & 0xff; p[1] = (s >> 8) & 0xff;
                p += 2;
            }
        }
        f.write(reinterpret_cast<const char*>(buf.data()), blockAlign);
    }
    if (!f) throw std::runtime_error("write failed: " + pathString(path));
}

} // namespace srt
