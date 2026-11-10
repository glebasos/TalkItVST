#pragma once
#include <tispeech/capi.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace talkit {

// Values the engine accepts (capi.h "Voice controls"); -1 keeps the
// personality's own default.
constexpr int kPersonalityCount = 20;
constexpr int kPitchMin = 10,  kPitchMax = 2000;
constexpr int kRateMin  = 20,  kRateMax  = 500;

enum class Voicing : int { Normal = 0, Breathy = 1, Whispered = 2 };

// capi.h allows 0..4. 1 is the engine's internal "style2"; the original UI
// never selected it, and 0/2/4 are what its three Pitch Quality buttons send
// (OpenTalkIt/README.md, confirmed against TIBASE32).
enum class F0Style : int { Natural = 0, Whispered = 1, Monotone = 2, Sung = 4 };

enum class Language : std::uint32_t {
    English = TISPEECH_LANG_ENGLISH,
    Spanish = TISPEECH_LANG_SPANISH,
};

struct VoiceSettings {
    Language language   = Language::English;
    int personality     = 0;
    int pitch           = -1;   // -1 = the personality's default
    int rate            = -1;
    Voicing voicing     = Voicing::Normal;
    F0Style f0Style     = F0Style::Natural;
    int f0Range         = -1;
    int f0Perturb       = -1;
    int vowelFactor     = -1;
    int glottalSource   = -1;

    tispeech_voice_options toOptions() const {
        return tispeech_voice_options{
            personality, pitch, rate, static_cast<int>(voicing),
            static_cast<int>(f0Style), f0Range, f0Perturb,
            vowelFactor, glottalSource};
    }
};

// The original Talk It!'s own 20 presets, as OpenTalkIt restored them
// (OpenTalkIt/Models/PersonalityPresets.cs). `pitch` and `rate` are raw engine
// values, not percentages: OpenTalkIt passes them straight through to
// tispeech_voice_options, so the same numbers reproduce the same voices.
struct Personality {
    std::string_view name;   // the label the original UI shows
    int pitch;
    int rate;
    F0Style f0Style;
    Voicing voicing;
};

inline constexpr std::array<Personality, kPersonalityCount> kPersonalities{{
    {"Man",            100, 150, F0Style::Natural,  Voicing::Normal},
    {"Woman",          200, 150, F0Style::Natural,  Voicing::Normal},
    {"Hyper Female",   190, 250, F0Style::Natural,  Voicing::Normal},
    {"Child",          350, 130, F0Style::Natural,  Voicing::Normal},
    {"Strong Man",      75, 140, F0Style::Natural,  Voicing::Normal},
    {"Mellow",         190, 140, F0Style::Natural,  Voicing::Breathy},
    {"Singing Girl",   310,  90, F0Style::Sung,     Voicing::Normal},
    {"Strong Woman",   200, 140, F0Style::Natural,  Voicing::Normal},
    {"Fly",            480, 150, F0Style::Natural,  Voicing::Normal},
    {"Little Robot",    90, 150, F0Style::Monotone, Voicing::Normal},
    {"Martian",         80, 150, F0Style::Monotone, Voicing::Normal},
    {"Big Robot",       66, 138, F0Style::Monotone, Voicing::Normal},
    {"Hyper Male",     135, 300, F0Style::Natural,  Voicing::Normal},
    {"Old Woman",      270, 115, F0Style::Natural,  Voicing::Normal},
    {"Little Man",      90, 150, F0Style::Natural,  Voicing::Normal},
    {"Imaginary Man",  110, 200, F0Style::Natural,  Voicing::Normal},
    {"Nerd",           140, 155, F0Style::Natural,  Voicing::Normal},
    {"Whiner",         120, 165, F0Style::Natural,  Voicing::Normal},
    {"Wobbly",         145, 115, F0Style::Sung,     Voicing::Normal},
    {"Singing Boy",    310,  90, F0Style::Sung,     Voicing::Normal},
}};

// Applies preset `index`, leaving language and the advanced fields alone.
inline void applyPersonality(VoiceSettings& v, int index) {
    if (index < 0 || index >= kPersonalityCount) return;
    const auto& p = kPersonalities[static_cast<size_t>(index)];
    v.personality = index;
    v.pitch       = p.pitch;
    v.rate        = p.rate;
    v.f0Style     = p.f0Style;
    v.voicing     = p.voicing;
}

}  // namespace talkit
