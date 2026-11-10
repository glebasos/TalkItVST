#pragma once
#include "Utterance.h"

#include <algorithm>
#include <cstdint>

namespace talkit {

// Plays a published Utterance. Audio-thread only: no locks, no allocation,
// no branches that depend on the host.
class Player {
public:
    void trigger() {
        m_position = 0;
        m_playing = true;
    }
    void stop() { m_playing = false; }
    bool playing() const { return m_playing; }
    std::int64_t position() const { return m_position; }

    void setLooping(bool loop) { m_looping = loop; }

    // Writes `frames` mono samples into `out` (replacing, not adding) and
    // returns how many were non-silent. A null or empty utterance is silence.
    int process(const Utterance* utterance, float* out, int frames, float gain) {
        if (frames <= 0) return 0;
        std::fill(out, out + frames, 0.0f);
        if (!m_playing || utterance == nullptr || utterance->samples.empty()) return 0;

        const auto total = static_cast<std::int64_t>(utterance->samples.size());
        // A re-render while playing hands us a different buffer. Clamping the
        // position keeps the voice going instead of clicking back to the start.
        if (m_position >= total) {
            if (!m_looping) {
                m_playing = false;
                return 0;
            }
            m_position = 0;
        }

        int written = 0;
        const float* src = utterance->samples.data();
        for (int i = 0; i < frames; ++i) {
            if (m_position >= total) {
                if (!m_looping) {
                    m_playing = false;
                    break;
                }
                m_position = 0;
            }
            out[i] = src[static_cast<size_t>(m_position)] * gain;
            ++m_position;
            ++written;
        }
        return written;
    }

private:
    std::int64_t m_position = 0;
    bool m_playing = false;
    bool m_looping = false;
};

}  // namespace talkit
