#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace talkit {

// Minimal 16-bit PCM WAV writer, for the command line tool and tests only.
inline bool writeWav(const std::string& path, const std::vector<float>& mono, int sampleRate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;

    const auto frames = static_cast<std::uint32_t>(mono.size());
    const std::uint32_t dataBytes = frames * 2;
    const std::uint32_t byteRate = static_cast<std::uint32_t>(sampleRate) * 2;

    auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);                                        // PCM
    u16(1);                                        // mono
    u32(static_cast<std::uint32_t>(sampleRate));
    u32(byteRate);
    u16(2);                                        // block align
    u16(16);                                       // bits
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);

    for (float s : mono) {
        const float clamped = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(clamped * 32767.0f)));
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

}  // namespace talkit
