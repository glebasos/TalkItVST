// Exercises the plugin core without a host: same Engine, same chunking, same
// resampler the plugin uses, so a problem heard here is a problem in the core
// rather than in the DAW integration.
//
//   talkit-render -t "Hello." -o out.wav
//   talkit-render --list
//   talkit-render --calibrate            (pitch value -> measured F0)

#include "../src/core/Engine.h"
#include "../src/core/Params.h"
#include "../src/core/Resampler.h"
#include "WavWrite.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

namespace {

using namespace talkit;

// Normalised autocorrelation over the steadiest part of the buffer. Enough to
// answer whether the engine's `pitch` value is a frequency in hertz.
std::optional<double> measureF0(const Rendered& r) {
    if (r.pcm.size() < 2048) return std::nullopt;

    // Skip the onset and the trailing silence, then take the loudest window.
    const size_t n = r.pcm.size();
    const size_t begin = n / 4, end = n - n / 4;
    std::vector<double> x;
    x.reserve(end - begin);
    double mean = 0.0;
    for (size_t i = begin; i < end; ++i) {
        const double v = static_cast<double>(r.pcm[i]) - 128.0;
        x.push_back(v);
        mean += v;
    }
    if (x.size() < 1024) return std::nullopt;
    mean /= static_cast<double>(x.size());
    for (auto& v : x) v -= mean;

    double energy = 0.0;
    for (double v : x) energy += v * v;
    if (energy < 1e-6) return std::nullopt;

    const auto minLag = static_cast<size_t>(r.sampleRate / 800.0);
    const auto maxLag = std::min(static_cast<size_t>(r.sampleRate / 50.0), x.size() / 2 - 1);
    if (maxLag <= minLag) return std::nullopt;

    std::vector<double> score(maxLag + 1, 0.0);
    double best = 0.0;
    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        double num = 0.0, den0 = 0.0, den1 = 0.0;
        for (size_t i = 0; i + lag < x.size(); ++i) {
            num += x[i] * x[i + lag];
            den0 += x[i] * x[i];
            den1 += x[i + lag] * x[i + lag];
        }
        const double denominator = std::sqrt(den0 * den1);
        if (denominator < 1e-9) continue;
        score[lag] = num / denominator;
        best = std::max(best, score[lag]);
    }
    if (best < 0.3) return std::nullopt;

    // Autocorrelation peaks just as hard at twice and three times the true
    // period, which reads as an octave or a fifth too low. Taking the shortest
    // lag that is nearly as good as the best one picks the real fundamental.
    for (size_t lag = minLag; lag <= maxLag; ++lag)
        if (score[lag] >= 0.85 * best)
            return static_cast<double>(r.sampleRate) / static_cast<double>(lag);
    return std::nullopt;
}

int listPersonalities() {
    std::cout << "idx  name            pitch  rate  pitch-quality  vocal-effort\n";
    for (size_t i = 0; i < kPersonalities.size(); ++i) {
        const auto& p = kPersonalities[i];
        std::cout << (i < 10 ? " " : "") << i << "   " << p.name;
        for (size_t pad = p.name.size(); pad < 16; ++pad) std::cout << ' ';
        std::cout << p.pitch << "    " << p.rate << "   "
                  << kF0StyleNames[p.f0Style == F0Style::Natural ? 0
                                   : p.f0Style == F0Style::Monotone ? 1 : 2]
                  << "        " << kVoicingNames[static_cast<int>(p.voicing)] << "\n";
    }
    return 0;
}

// Is the engine's `pitch` field a frequency? The restored presets hint at it
// (Man 100, Woman 200, Child 350), and the instrument phase needs a real
// answer to map MIDI notes onto it.
int calibrate() {
    VoiceSettings voice;
    voice.f0Style = F0Style::Monotone;   // a steady F0 is measurable
    voice.rate = 90;                     // slow, so the vowel is long

    std::cout << "pitch  measured F0   ratio\n";
    for (int pitch : {50, 66, 75, 90, 100, 120, 150, 200, 250, 300, 350, 400, 480, 600, 800}) {
        voice.pitch = pitch;
        Rendered r;
        std::string error;
        if (!Engine::render("aaaaaaaaah", voice, r, error)) {
            std::cout << pitch << "  render failed: " << error << "\n";
            continue;
        }
        const auto f0 = measureF0(r);
        std::cout << (pitch < 100 ? " " : "") << pitch << "    ";
        if (f0) {
            std::printf("%8.1f Hz  %6.3f\n", *f0, *f0 / pitch);
        } else {
            std::cout << "   unvoiced\n";
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string text = "Hello from nineteen ninety three.";
    std::string output;
    int hostRate = 48000;
    auto interpolation = Interpolation::Clean;
    VoiceSettings voice;
    applyPersonality(voice, 0);
    bool explicitPitch = false, explicitRate = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

        if (a == "--list") return listPersonalities();
        if (a == "--calibrate") return calibrate();
        if (a == "-t" || a == "--text") text = next();
        else if (a == "-o" || a == "--out") output = next();
        else if (a == "--personality") applyPersonality(voice, std::atoi(next().c_str()));
        else if (a == "--pitch") { voice.pitch = std::atoi(next().c_str()); explicitPitch = true; }
        else if (a == "--rate") { voice.rate = std::atoi(next().c_str()); explicitRate = true; }
        else if (a == "--voicing") voice.voicing = static_cast<Voicing>(std::atoi(next().c_str()));
        else if (a == "--style") voice.f0Style = static_cast<F0Style>(std::atoi(next().c_str()));
        else if (a == "--spanish") voice.language = Language::Spanish;
        else if (a == "--host-rate") hostRate = std::atoi(next().c_str());
        else if (a == "--crunch") interpolation = Interpolation::Crunch;
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: talkit-render [-t TEXT] [-o OUT.wav] [--personality 0-19]\n"
                         "                     [--pitch N] [--rate N] [--voicing 0-2]\n"
                         "                     [--style 0|2|4] [--spanish] [--host-rate HZ]\n"
                         "                     [--crunch] [--list] [--calibrate]\n";
            return 0;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        }
    }
    (void)explicitPitch;
    (void)explicitRate;

    std::cout << "engine: " << Engine::buildInfo() << "\n";
    if (!Engine::canSynthesize(voice.language)) {
        std::cerr << "this build cannot synthesize the selected language\n";
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    Rendered rendered;
    std::string error;
    if (!Engine::render(text, voice, rendered, error)) {
        std::cerr << "render failed: " << error << "\n";
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto utterance = resample(rendered, hostRate, interpolation);
    const auto t2 = std::chrono::steady_clock::now();

    const auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::printf("chunks:    %zu\n", Engine::chunk(text).size());
    std::printf("engine:    %d samples @ %d Hz (%.2f s)\n",
                static_cast<int>(rendered.pcm.size()), rendered.sampleRate,
                rendered.seconds());
    std::printf("events:    %zu\n", rendered.events.size());
    std::printf("resampled: %lld frames @ %d Hz (%.2f s)\n",
                static_cast<long long>(utterance.frames()), utterance.sampleRate,
                utterance.seconds());
    std::printf("synthesis: %.1f ms  (%.0fx realtime)\n", ms(t0, t1),
                rendered.seconds() * 1000.0 / std::max(ms(t0, t1), 0.001));
    std::printf("resample:  %.1f ms\n", ms(t1, t2));

    if (!output.empty()) {
        if (!writeWav(output, utterance.samples, utterance.sampleRate)) {
            std::cerr << "could not write " << output << "\n";
            return 1;
        }
        std::cout << "wrote " << output << "\n";
    }
    return 0;
}
