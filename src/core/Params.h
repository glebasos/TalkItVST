#pragma once
#include "Resampler.h"
#include "Voice.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace talkit {

// Stable ids: they go into saved projects, so never renumber them.
enum class ParamId : std::uint32_t {
    Personality   = 0,
    Pitch         = 1,
    Rate          = 2,
    Voicing       = 3,
    F0Style       = 4,
    Language      = 5,
    Interpolation = 6,
    Gain          = 7,
    Loop          = 8,
    SyncTransport = 9,
};

struct ParamInfo {
    ParamId id;
    std::string_view name;
    std::string_view module;
    double min, max, def;
    bool stepped;
};

inline constexpr std::array<ParamInfo, 10> kParams{{
    {ParamId::Personality,   "Personality",   "Voice",  0, kPersonalityCount - 1, 0,   true},
    {ParamId::Pitch,         "Pitch",         "Voice",  kPitchMin, kPitchMax,     100, true},
    {ParamId::Rate,          "Rate",          "Voice",  kRateMin,  kRateMax,      150, true},
    {ParamId::Voicing,       "Vocal Effort",  "Voice",  0, 2,                     0,   true},
    {ParamId::F0Style,       "Pitch Quality", "Voice",  0, 2,                     0,   true},
    {ParamId::Language,      "Language",      "Voice",  0, 1,                     0,   true},
    {ParamId::Interpolation, "Resampling",    "Output", 0, 1,                     0,   true},
    {ParamId::Gain,          "Gain",          "Output", 0.0, 2.0,                 1.0, false},
    {ParamId::Loop,          "Loop",          "Output", 0, 1,                     0,   true},
    {ParamId::SyncTransport, "Speak On Play", "Output", 0, 1,                     0,   true},
}};

// Pitch Quality exposes the original UI's three choices; the engine's style 1
// is internal and never selectable (see Voice.h).
inline constexpr std::array<F0Style, 3> kF0StyleChoices{
    F0Style::Natural, F0Style::Monotone, F0Style::Sung};

inline constexpr std::array<std::string_view, 3> kF0StyleNames{
    "Natural", "Monotone", "Sung"};
inline constexpr std::array<std::string_view, 3> kVoicingNames{
    "Normal", "Breathy", "Whispered"};
inline constexpr std::array<std::string_view, 2> kLanguageNames{
    "English", "Spanish"};
inline constexpr std::array<std::string_view, 2> kInterpolationNames{
    "Clean", "Crunch"};

inline const ParamInfo* findParam(ParamId id) {
    for (const auto& p : kParams)
        if (p.id == id) return &p;
    return nullptr;
}

}  // namespace talkit
