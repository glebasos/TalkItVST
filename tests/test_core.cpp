#include "../src/core/Engine.h"
#include "../src/core/Latin1.h"
#include "../src/core/Player.h"
#include "../src/core/RenderQueue.h"
#include "../src/core/Resampler.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <thread>

using namespace talkit;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "FAIL  ", what);
    if (!ok) ++failures;
}

void testLatin1() {
    check(utf8ToLatin1("plain ascii") == "plain ascii", "latin1: ascii unchanged");
    check(utf8ToLatin1("\xe2\x80\x9cquoted\xe2\x80\x9d") == "\"quoted\"",
          "latin1: curly quotes become straight");
    check(utf8ToLatin1("wait\xe2\x80\xa6 now") == "wait... now", "latin1: ellipsis expands");
    check(utf8ToLatin1("a\xe2\x80\x94" "b") == "a-b", "latin1: em dash becomes hyphen");
    check(utf8ToLatin1("caf\xc3\xa9") == "caf\xe9", "latin1: accented char maps to one byte");
    check(utf8ToLatin1("\xe6\x97\xa5") == "?", "latin1: outside latin-1 becomes ?");
    check(utf8ToLatin1("bad\xff\xfeinput").size() > 0, "latin1: malformed input survives");
}

void testChunking() {
    const auto one = Engine::chunk("Short enough.", 514);
    check(one.size() == 1 && one[0] == "Short enough.", "chunk: short text is one piece");

    std::string sentences;
    for (int i = 0; i < 60; ++i) sentences += "This is sentence number " + std::to_string(i) + ". ";
    const auto many = Engine::chunk(sentences, 100);
    bool withinLimit = !many.empty();
    for (const auto& c : many) withinLimit = withinLimit && c.size() <= 100;
    check(withinLimit, "chunk: every piece is within the limit");

    bool endsAtSentence = true;
    for (size_t i = 0; i + 1 < many.size(); ++i)
        endsAtSentence = endsAtSentence && many[i].back() == '.';
    check(endsAtSentence, "chunk: pieces end at sentence boundaries");

    // No text may be lost; only whitespace at the joins.
    std::string rejoined;
    for (const auto& c : many) rejoined += c + " ";
    auto strip = [](std::string s) {
        std::string out;
        for (char ch : s)
            if (ch != ' ') out += ch;
        return out;
    };
    check(strip(rejoined) == strip(sentences), "chunk: no characters are dropped");

    const std::string unbroken(1200, 'x');
    const auto forced = Engine::chunk(unbroken, 514);
    bool forcedOk = forced.size() == 3;
    for (const auto& c : forced) forcedOk = forcedOk && c.size() <= 514;
    check(forcedOk, "chunk: one unbroken run is split mid-word");

    check(Engine::chunk("", 514).empty(), "chunk: empty text yields nothing");
    check(Engine::chunk("   \n\t ", 514).empty(), "chunk: whitespace only yields nothing");
}

// capi.h documents 514 characters per call. If a future engine build changes
// that, kMaxCharsPerCall has to change with it, so pin it down here.
void testEngineLimit() {
    std::vector<char> phonemes(16384);
    auto accepts = [&](size_t n) {
        const std::string text(n, 'a');
        return tispeech_text_to_phonemes(TISPEECH_LANG_ENGLISH, text.c_str(),
                                         phonemes.data(),
                                         static_cast<std::int32_t>(phonemes.size())) ==
               TISPEECH_OK;
    };
    check(accepts(kMaxCharsPerCall), "engine: accepts kMaxCharsPerCall characters");
    check(!accepts(kMaxCharsPerCall + 1),
          "engine: rejects one more, so the limit is still 514");
}

void testResampler() {
    Rendered src;
    src.sampleRate = 11025;
    src.pcm.assign(11025, 128);   // digital silence in unsigned 8-bit
    const auto silent = resample(src, 48000, Interpolation::Clean);
    check(silent.sampleRate == 48000, "resample: output carries the target rate");
    check(std::llabs(silent.frames() - 48000) <= 2, "resample: length follows the ratio");
    float peak = 0.0f;
    for (float s : silent.samples) peak = std::max(peak, std::abs(s));
    check(peak < 1e-4f, "resample: silence stays silent");

    // A 440 Hz tone must come back as 440 Hz at roughly the same level.
    for (size_t i = 0; i < src.pcm.size(); ++i) {
        const double t = static_cast<double>(i) / src.sampleRate;
        const double v = std::sin(2.0 * std::numbers::pi * 440.0 * t);
        src.pcm[i] = static_cast<std::uint8_t>(std::lround(128.0 + v * 100.0));
    }
    const auto tone = resample(src, 48000, Interpolation::Clean);
    double rms = 0.0;
    for (float s : tone.samples) rms += static_cast<double>(s) * s;
    rms = std::sqrt(rms / static_cast<double>(tone.samples.size()));
    const double expected = (100.0 / 127.5) / std::sqrt(2.0);
    check(std::abs(rms - expected) < 0.05, "resample: tone level is preserved");

    int crossings = 0;
    for (size_t i = 1; i < tone.samples.size(); ++i)
        if ((tone.samples[i - 1] < 0.0f) != (tone.samples[i] < 0.0f)) ++crossings;
    check(std::abs(crossings / 2 - 440) < 8, "resample: tone frequency is preserved");

    src.events.push_back(Event{11025 / 2, TISPEECH_EVENT_WORD, 0});
    const auto withEvent = resample(src, 48000, Interpolation::Clean);
    check(withEvent.events.size() == 1 &&
              std::llabs(withEvent.events[0].sample - 24000) < 4,
          "resample: event offsets are rebased");

    const auto crunch = resample(src, 48000, Interpolation::Crunch);
    check(std::llabs(crunch.frames() - tone.frames()) <= 2,
          "resample: crunch mode matches length");
}

void testEngineRender() {
    VoiceSettings voice;
    applyPersonality(voice, 0);
    Rendered r;
    std::string error;
    const bool ok = Engine::render("Hello, world. This is a test.", voice, r, error);
    check(ok, ok ? "engine: renders English" : error.c_str());
    check(r.sampleRate == 11025, "engine: output is 11025 Hz");
    check(r.seconds() > 0.5 && r.seconds() < 10.0, "engine: duration is plausible");
    check(!r.events.empty(), "engine: reports events");

    bool ordered = true;
    for (size_t i = 1; i < r.events.size(); ++i)
        ordered = ordered && r.events[i - 1].sample <= r.events[i].sample;
    check(ordered, "engine: events are in order");
    bool inRange = true;
    for (const auto& e : r.events)
        inRange = inRange && e.sample >= 0 &&
                  e.sample < static_cast<std::int64_t>(r.pcm.size());
    check(inRange, "engine: event offsets land inside the buffer");

    // Multi-chunk text must join without losing audio.
    std::string longText;
    for (int i = 0; i < 40; ++i) longText += "Sentence number " + std::to_string(i) + ". ";
    check(Engine::chunk(longText).size() > 1, "engine: long text needs several chunks");
    Rendered joined;
    const bool okLong = Engine::render(longText, voice, joined, error);
    check(okLong, okLong ? "engine: renders multi-chunk text" : error.c_str());
    check(joined.seconds() > r.seconds(), "engine: joined audio is longer");

    voice.language = Language::Spanish;
    Rendered spanish;
    check(Engine::render("Hola, buenos dias.", voice, spanish, error),
          "engine: renders Spanish");

    voice.language = Language::English;
    Rendered empty;
    check(!Engine::render("", voice, empty, error), "engine: empty text is an error");
}

void testQueue() {
    RenderQueue queue;
    VoiceSettings voice;
    applyPersonality(voice, 1);
    queue.setSampleRate(48000);
    queue.setVoice(voice);
    queue.setText("Queued speech.");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (queue.rendering() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    check(!queue.rendering(), "queue: render completes");
    check(queue.lastError().empty(),
          queue.lastError().empty() ? "queue: no error" : queue.lastError().c_str());

    auto hold = queue.acquire();
    check(static_cast<bool>(hold), "queue: publishes an utterance");
    if (hold) {
        check(hold->sampleRate == 48000, "queue: utterance is at the host rate");
        check(!hold->empty(), "queue: utterance has audio");

        Player player;
        std::vector<float> block(512, 1.0f);
        player.process(&*hold, block.data(), 512, 1.0f);
        bool silentBeforeTrigger = true;
        for (float s : block) silentBeforeTrigger = silentBeforeTrigger && s == 0.0f;
        check(silentBeforeTrigger, "player: silent until triggered");

        player.trigger();
        const int written = player.process(&*hold, block.data(), 512, 1.0f);
        check(written == 512, "player: fills the block once triggered");
        check(player.position() == 512, "player: advances by the block size");

        // Run past the end: it must stop rather than read out of bounds.
        const auto total = hold->frames();
        for (std::int64_t done = 512; done < total + 2048; done += 512)
            player.process(&*hold, block.data(), 512, 1.0f);
        check(!player.playing(), "player: stops at the end of the phrase");
    }
    hold = RenderQueue::Hold{};

    // Parameter sweeps arrive from the audio thread; the worker must keep up
    // without tearing or deadlocking.
    for (int i = 0; i < 200; ++i) {
        voice.pitch = 80 + i;
        queue.setVoice(voice);
    }
    const auto sweepDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (queue.rendering() && std::chrono::steady_clock::now() < sweepDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(!queue.rendering(), "queue: survives a parameter sweep");
    auto after = queue.acquire();
    check(static_cast<bool>(after) && !after->empty(), "queue: still has audio after the sweep");
}

}  // namespace

int main() {
    std::printf("engine: %s\n\n", Engine::buildInfo().c_str());
    testLatin1();
    testChunking();
    testEngineLimit();
    testResampler();
    testEngineRender();
    testQueue();
    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : (std::to_string(failures) + " failed").c_str());
    return failures == 0 ? 0 : 1;
}
