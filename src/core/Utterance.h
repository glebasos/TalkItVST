#pragma once
#include "Engine.h"

#include <cstdint>
#include <vector>

namespace talkit {

// A rendered phrase, resampled to the host's rate and ready for the audio
// thread. Immutable once published: the audio thread only reads it.
struct Utterance {
    std::vector<float> samples;      // mono, -1..1, host sample rate
    int sampleRate = 0;
    std::vector<Event> events;       // sample offsets rebased to sampleRate
    std::uint64_t revision = 0;      // which render request produced this

    bool empty() const { return samples.empty(); }
    std::int64_t frames() const { return static_cast<std::int64_t>(samples.size()); }
    double seconds() const {
        return sampleRate > 0 ? static_cast<double>(samples.size()) / sampleRate : 0.0;
    }
};

}  // namespace talkit
