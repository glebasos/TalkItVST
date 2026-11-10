#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace talkit {
namespace {

constexpr int kLanczosA = 3;

float sinc(double x) {
    if (std::abs(x) < 1e-9) return 1.0f;
    const double px = std::numbers::pi * x;
    return static_cast<float>(std::sin(px) / px);
}

// Lanczos window, widened by `scale` when decimating so the kernel band-limits
// to the *output* Nyquist rather than the input's.
float lanczos(double x, double scale) {
    const double sx = x / scale;
    if (std::abs(sx) >= kLanczosA) return 0.0f;
    return sinc(sx) * sinc(sx / kLanczosA) / static_cast<float>(scale);
}

// 8-bit unsigned PCM is centred on 128; 127.5 maps 0 and 255 to -1 and +1.
float toFloat(std::uint8_t s) {
    return (static_cast<float>(s) - 128.0f) / 127.5f;
}

float sampleAt(const std::vector<std::uint8_t>& pcm, std::int64_t i) {
    if (pcm.empty()) return 0.0f;
    i = std::clamp<std::int64_t>(i, 0, static_cast<std::int64_t>(pcm.size()) - 1);
    return toFloat(pcm[static_cast<size_t>(i)]);
}

}  // namespace

Utterance resample(const Rendered& src, int dstRate, Interpolation mode) {
    Utterance out;
    out.sampleRate = dstRate;
    if (src.pcm.empty() || src.sampleRate <= 0 || dstRate <= 0) return out;

    const double ratio = static_cast<double>(src.sampleRate) / dstRate;
    const auto inN = static_cast<std::int64_t>(src.pcm.size());
    const auto outN = static_cast<std::int64_t>(std::llround(inN / ratio));
    if (outN <= 0) return out;

    out.samples.resize(static_cast<size_t>(outN));

    if (mode == Interpolation::Crunch) {
        for (std::int64_t i = 0; i < outN; ++i) {
            const auto nearest = static_cast<std::int64_t>(std::llround(i * ratio));
            out.samples[static_cast<size_t>(i)] = sampleAt(src.pcm, nearest);
        }
    } else {
        const double scale = std::max(1.0, ratio);
        const auto half = static_cast<std::int64_t>(std::ceil(kLanczosA * scale));
        for (std::int64_t i = 0; i < outN; ++i) {
            const double t = i * ratio;
            const auto centre = static_cast<std::int64_t>(std::floor(t));
            float acc = 0.0f, weight = 0.0f;
            for (std::int64_t k = centre - half + 1; k <= centre + half; ++k) {
                const float w = lanczos(t - static_cast<double>(k), scale);
                if (w == 0.0f) continue;
                acc += w * sampleAt(src.pcm, k);
                weight += w;
            }
            // Normalising keeps unity gain despite the truncated kernel, so a
            // sustained vowel does not ripple in level.
            out.samples[static_cast<size_t>(i)] =
                weight != 0.0f ? acc / weight : 0.0f;
        }
    }

    out.events.reserve(src.events.size());
    for (const auto& e : src.events) {
        auto frame = static_cast<std::int64_t>(std::llround(e.sample / ratio));
        frame = std::clamp<std::int64_t>(frame, 0, outN - 1);
        out.events.push_back(Event{frame, e.code, e.value});
    }
    return out;
}

}  // namespace talkit
