#include "Engine.h"

#include "Latin1.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace talkit {
namespace {

bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// A sentence end only counts when what follows is whitespace or the end of
// the text, so "3.5" and "Dr." mid-sentence do not split a chunk.
bool isSentenceEnd(std::string_view s, size_t i) {
    const char c = s[i];
    if (c != '.' && c != '!' && c != '?' && c != ';' && c != ':') return false;
    return i + 1 >= s.size() || isSpace(static_cast<unsigned char>(s[i + 1]));
}

struct SamplesDeleter {
    void operator()(std::uint8_t* p) const { tispeech_free_samples(p); }
};
struct EventsDeleter {
    void operator()(tispeech_event* p) const { tispeech_free_events(p); }
};

}  // namespace

std::string Engine::buildInfo() {
    const char* s = tispeech_build_info();
    return s ? s : "";
}

bool Engine::canSynthesize(Language language) {
    if ((tispeech_capabilities() & TISPEECH_CAP_SYNTHESIS) == 0) return false;
    return (tispeech_synthesis_languages() & static_cast<std::uint32_t>(language)) != 0;
}

std::string Engine::describeError(std::int32_t rc) {
    switch (rc) {
        case TISPEECH_OK:             return "ok";
        case TISPEECH_E_BADPARAM:     return "bad parameter";
        case TISPEECH_E_NOLANGUAGE:   return "this build has no data for that language";
        case TISPEECH_E_NULLTEXT:     return "empty text";
        case TISPEECH_E_DICTSHORT:    return "user dictionary truncated";
        case TISPEECH_E_DICTFORMAT:   return "not an SVXF user dictionary";
        case TISPEECH_E_OUTOFMEMORY:  return "out of memory";
        case TISPEECH_E_NOTIMPL:      return "not supported by this build";
        case TISPEECH_E_BUFFERFULL:   return "phoneme buffer too small";
        case TISPEECH_E_BADDLL:       return "unreadable or unsupported original DLL";
        default:                      return "engine error " + std::to_string(rc);
    }
}

std::vector<std::string> Engine::chunk(std::string_view s, size_t limit) {
    std::vector<std::string> out;
    if (limit == 0) return out;

    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && isSpace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) break;

        if (s.size() - pos <= limit) {
            out.emplace_back(s.substr(pos));
            break;
        }

        const size_t window = pos + limit;
        size_t cut = 0;
        for (size_t i = window; i > pos; --i) {
            if (isSentenceEnd(s, i - 1)) { cut = i; break; }
        }
        if (cut == 0) {
            for (size_t i = window; i > pos; --i) {
                if (isSpace(static_cast<unsigned char>(s[i - 1]))) { cut = i - 1; break; }
            }
        }
        if (cut <= pos) cut = window;   // one unbroken run longer than the limit

        auto piece = s.substr(pos, cut - pos);
        while (!piece.empty() && isSpace(static_cast<unsigned char>(piece.back())))
            piece.remove_suffix(1);
        if (!piece.empty()) out.emplace_back(piece);
        pos = cut;
    }
    return out;
}

bool Engine::render(std::string_view utf8, const VoiceSettings& voice,
                    Rendered& out, std::string& error) {
    out = Rendered{};
    error.clear();

    const auto language = static_cast<std::uint32_t>(voice.language);
    if (!canSynthesize(voice.language)) {
        error = "this build cannot synthesize the selected language";
        return false;
    }

    const std::string latin1 = utf8ToLatin1(utf8);
    const auto pieces = chunk(latin1);
    if (pieces.empty()) {
        error = "nothing to speak";
        return false;
    }

    const auto options = voice.toOptions();
    constexpr std::uint32_t kEvents = TISPEECH_EVENTS_SENTENCE | TISPEECH_EVENTS_SYLLABLE |
                                      TISPEECH_EVENTS_PHONEME | TISPEECH_EVENTS_MOUTH;

    // Grows to fit the longest phoneme string a 514-char piece can produce.
    std::vector<char> phonemes(8192);

    for (const auto& piece : pieces) {
        std::int32_t rc = TISPEECH_E_BUFFERFULL;
        for (int attempt = 0; attempt < 4 && rc == TISPEECH_E_BUFFERFULL; ++attempt) {
            rc = tispeech_text_to_phonemes(language, piece.c_str(), phonemes.data(),
                                           static_cast<std::int32_t>(phonemes.size()));
            if (rc == TISPEECH_E_BUFFERFULL) phonemes.resize(phonemes.size() * 2);
        }
        if (rc != TISPEECH_OK) {
            error = "text to phonemes: " + describeError(rc);
            out = Rendered{};
            return false;
        }

        std::uint8_t* rawPcm = nullptr;
        tispeech_event* rawEvents = nullptr;
        std::int32_t count = 0, rate = 0, eventCount = 0;
        rc = tispeech_synthesize_events(language, phonemes.data(), &options, kEvents,
                                        &rawPcm, &count, &rate, &rawEvents, &eventCount);
        std::unique_ptr<std::uint8_t, SamplesDeleter> pcmOwner(rawPcm);
        std::unique_ptr<tispeech_event, EventsDeleter> eventOwner(rawEvents);

        if (rc != TISPEECH_OK) {
            error = "synthesize: " + describeError(rc);
            out = Rendered{};
            return false;
        }
        if (out.sampleRate == 0) {
            out.sampleRate = rate;
        } else if (out.sampleRate != rate) {
            error = "engine changed sample rate between chunks";
            out = Rendered{};
            return false;
        }

        const auto base = static_cast<std::int64_t>(out.pcm.size());
        if (count > 0 && rawPcm) out.pcm.insert(out.pcm.end(), rawPcm, rawPcm + count);
        out.events.reserve(out.events.size() + static_cast<size_t>(std::max(eventCount, 0)));
        for (std::int32_t i = 0; i < eventCount; ++i) {
            out.events.push_back(Event{base + rawEvents[i].sample,
                                       rawEvents[i].code, rawEvents[i].value});
        }
    }

    if (out.pcm.empty()) {
        error = "the engine produced no audio";
        return false;
    }
    return true;
}

}  // namespace talkit
