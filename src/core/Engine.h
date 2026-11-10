#pragma once
#include "Voice.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace talkit {

// capi.h: "Input is limited to 514 Latin-1 characters per call." Longer input
// is an explicit error, so anything bigger has to be split. tests/test_core
// asserts this is still the real boundary.
constexpr size_t kMaxCharsPerCall = 514;

struct Event {
    std::int64_t sample;   // offset into Rendered::pcm, chunks already summed
    std::uint16_t code;    // TISPEECH_EVENT_*
    std::uint16_t value;
};

// The engine's native output: 8-bit unsigned mono, 11025 Hz.
struct Rendered {
    std::vector<std::uint8_t> pcm;
    int sampleRate = 0;
    std::vector<Event> events;

    bool empty() const { return pcm.empty(); }
    double seconds() const {
        return sampleRate > 0 ? static_cast<double>(pcm.size()) / sampleRate : 0.0;
    }
};

// Wraps libtispeech. Every call allocates and takes milliseconds, so this is
// worker-thread-only: never call it from an audio callback. It holds no state
// of its own, and with the data compiled in the engine's only mutable global
// is the run-time DLL loader we never touch, so concurrent renders are safe.
class Engine {
public:
    static std::string buildInfo();
    static bool canSynthesize(Language);

    // Converts to Latin-1, splits at sentence boundaries to respect the
    // per-call limit, synthesizes each piece and concatenates. Event sample
    // offsets are rebased onto the joined buffer. False on failure.
    static bool render(std::string_view utf8, const VoiceSettings&,
                       Rendered& out, std::string& error);

    // Splits Latin-1 text into pieces of at most `limit` characters, breaking
    // at a sentence end where possible, otherwise a word boundary, otherwise
    // mid-word. Exposed for testing.
    static std::vector<std::string> chunk(std::string_view latin1,
                                          size_t limit = kMaxCharsPerCall);

    static std::string describeError(std::int32_t rc);
};

}  // namespace talkit
