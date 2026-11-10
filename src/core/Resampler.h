#pragma once
#include "Utterance.h"

namespace talkit {

enum class Interpolation {
    // Lanczos-3 windowed sinc. The engine's 11025 Hz band is reproduced
    // without adding imaging above it; the 8-bit quantisation noise already
    // in the source is left alone, so the voice still sounds like 1993.
    Clean,
    // Nearest sample, i.e. what naive playback of an 11 kHz buffer at 48 kHz
    // does. Adds the stair-step brightness and aliasing some people want from
    // a retro plugin. Deliberate, not an accident.
    Crunch,
};

// Converts the engine's 8-bit unsigned 11025 Hz output to float at `dstRate`.
// Pure and allocating: worker thread only.
Utterance resample(const Rendered& src, int dstRate, Interpolation mode);

}  // namespace talkit
