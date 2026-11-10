#include "RenderQueue.h"

#include "Engine.h"

#include <chrono>

namespace talkit {

RenderQueue::RenderQueue() { m_worker = std::thread([this] { run(); }); }

RenderQueue::~RenderQueue() {
    m_stop.store(true, std::memory_order_release);
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
    m_live.store(nullptr, std::memory_order_release);
}

void RenderQueue::Hold::release() {
    if (m_queue) {
        m_queue->m_readers.fetch_sub(1, std::memory_order_release);
        m_queue = nullptr;
        m_utterance = nullptr;
    }
}

RenderQueue::Hold RenderQueue::acquire() {
    m_readers.fetch_add(1, std::memory_order_acquire);
    const Utterance* u = m_live.load(std::memory_order_acquire);
    if (u == nullptr) {
        m_readers.fetch_sub(1, std::memory_order_release);
        return Hold{};
    }
    return Hold{this, u};
}

void RenderQueue::setText(std::string utf8) {
    {
        std::lock_guard lock(m_textMutex);
        if (m_text == utf8) return;
        m_text = std::move(utf8);
    }
    requestRender();
}

void RenderQueue::setInterpolation(Interpolation mode) {
    {
        std::lock_guard lock(m_textMutex);
        if (m_interpolation == mode) return;
        m_interpolation = mode;
    }
    requestRender();
}

std::string RenderQueue::text() const {
    std::lock_guard lock(m_textMutex);
    return m_text;
}

void RenderQueue::setVoice(const VoiceSettings& v) {
    // Seqlock write. Odd sequence means "being written"; the worker retries.
    const auto seq = m_voice.seq.load(std::memory_order_relaxed);
    m_voice.seq.store(seq + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    m_voice.value = v;
    std::atomic_thread_fence(std::memory_order_release);
    m_voice.seq.store(seq + 2, std::memory_order_release);
    requestRender();
}

void RenderQueue::setSampleRate(int rate) {
    if (rate <= 0) return;
    if (m_voice.sampleRate.exchange(rate, std::memory_order_release) == rate) return;
    requestRender();
}

void RenderQueue::requestRender() {
    m_requested.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
}

std::string RenderQueue::lastError() const {
    std::lock_guard lock(m_errorMutex);
    return m_error;
}

void RenderQueue::publish(std::unique_ptr<Utterance> next) {
    const Utterance* previous = m_live.load(std::memory_order_acquire);
    m_live.store(next.get(), std::memory_order_release);
    if (m_current && m_current.get() == previous) m_retired.push_back(std::move(m_current));
    m_current = std::move(next);
}

void RenderQueue::collectRetired() {
    if (m_retired.empty()) return;
    // A reader count of zero means no audio callback holds any pointer, so
    // everything retired before now is unreachable. The audio thread holds a
    // pointer for one callback only, so this drains in well under a buffer.
    for (int spin = 0; spin < 200; ++spin) {
        if (m_readers.load(std::memory_order_acquire) == 0) {
            m_retired.clear();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Still busy: keep them and try after the next render rather than risk a
    // use-after-free. The list holds one utterance per render, so it is bounded
    // by how fast parameters are being swept.
}

void RenderQueue::run() {
    std::uint64_t done = 0;

    while (!m_stop.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(m_wakeMutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(100), [this, &done] {
                return m_stop.load(std::memory_order_acquire) ||
                       m_requested.load(std::memory_order_acquire) != done;
            });
        }
        if (m_stop.load(std::memory_order_acquire)) break;

        const auto wanted = m_requested.load(std::memory_order_acquire);
        if (wanted == done) {
            collectRetired();
            continue;
        }

        std::string utf8;
        Interpolation interpolation;
        {
            std::lock_guard lock(m_textMutex);
            utf8 = m_text;
            interpolation = m_interpolation;
        }

        VoiceSettings voice;
        for (int attempt = 0;; ++attempt) {
            const auto before = m_voice.seq.load(std::memory_order_acquire);
            if ((before & 1u) == 0) {
                std::atomic_thread_fence(std::memory_order_acquire);
                voice = m_voice.value;
                std::atomic_thread_fence(std::memory_order_acquire);
                if (m_voice.seq.load(std::memory_order_acquire) == before) break;
            }
            if (attempt > 1000) break;   // a torn read is better than a hang
            std::this_thread::yield();
        }
        const int sampleRate = m_voice.sampleRate.load(std::memory_order_acquire);

        if (utf8.empty()) {
            publish(std::make_unique<Utterance>());
            {
                std::lock_guard lock(m_errorMutex);
                m_error.clear();
            }
            done = wanted;
            m_published.store(done, std::memory_order_release);
            collectRetired();
            continue;
        }

        Rendered rendered;
        std::string error;
        const bool ok = Engine::render(utf8, voice, rendered, error);

        // A newer request landed while we were rendering: drop this result and
        // start again, so a parameter sweep never queues a backlog of stale
        // audio.
        if (m_requested.load(std::memory_order_acquire) != wanted) continue;

        {
            std::lock_guard lock(m_errorMutex);
            m_error = ok ? std::string{} : error;
        }
        if (ok) {
            auto utterance = std::make_unique<Utterance>(
                resample(rendered, sampleRate, interpolation));
            utterance->revision = wanted;
            publish(std::move(utterance));
        }

        done = wanted;
        m_published.store(done, std::memory_order_release);
        collectRetired();
    }
}

}  // namespace talkit
