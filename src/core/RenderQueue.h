#pragma once
#include "Resampler.h"
#include "Utterance.h"
#include "Voice.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace talkit {

// Owns the one worker thread that calls the engine, and hands finished
// utterances to the audio thread without locks or allocation on that side.
//
// Three callers, three contracts:
//   setText / setInterpolation  - GUI or state thread, may block
//   setVoice / setSampleRate    - audio thread, lock-free (CLAP delivers
//                                 parameter changes inside process())
//   acquire                     - audio thread, lock-free
class RenderQueue {
public:
    RenderQueue();
    ~RenderQueue();
    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;

    // Borrows the published utterance for the length of one audio callback.
    // Keeps the worker from freeing it while in use; never blocks.
    class Hold {
    public:
        Hold() = default;
        ~Hold() { release(); }
        Hold(Hold&& o) noexcept : m_queue(o.m_queue), m_utterance(o.m_utterance) {
            o.m_queue = nullptr;
            o.m_utterance = nullptr;
        }
        Hold& operator=(Hold&& o) noexcept {
            if (this != &o) {
                release();
                m_queue = o.m_queue;
                m_utterance = o.m_utterance;
                o.m_queue = nullptr;
                o.m_utterance = nullptr;
            }
            return *this;
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

        const Utterance* operator->() const { return m_utterance; }
        const Utterance& operator*() const { return *m_utterance; }
        explicit operator bool() const { return m_utterance != nullptr; }

    private:
        friend class RenderQueue;
        Hold(RenderQueue* q, const Utterance* u) : m_queue(q), m_utterance(u) {}
        void release();

        RenderQueue* m_queue = nullptr;
        const Utterance* m_utterance = nullptr;
    };

    void setText(std::string utf8);
    void setInterpolation(Interpolation);
    std::string text() const;

    void setVoice(const VoiceSettings&);   // audio thread
    void setSampleRate(int);               // audio thread
    Hold acquire();                        // audio thread

    // Revision of the newest request, and of what is actually published.
    std::uint64_t requestedRevision() const { return m_requested.load(std::memory_order_acquire); }
    std::uint64_t publishedRevision() const { return m_published.load(std::memory_order_acquire); }
    bool rendering() const { return requestedRevision() != publishedRevision(); }

    // Last failure, if any. GUI thread.
    std::string lastError() const;

private:
    void run();
    void requestRender();
    void publish(std::unique_ptr<Utterance>);
    void collectRetired();

    // Voice settings written by the audio thread, read by the worker. A
    // seqlock, because the writer must not block and the payload is too big
    // for one atomic.
    struct VoiceSlot {
        std::atomic<std::uint32_t> seq{0};
        VoiceSettings value{};
        std::atomic<int> sampleRate{44100};
    };
    VoiceSlot m_voice;

    mutable std::mutex m_textMutex;
    std::string m_text;
    Interpolation m_interpolation = Interpolation::Clean;

    mutable std::mutex m_errorMutex;
    std::string m_error;

    std::atomic<std::uint64_t> m_requested{0};
    std::atomic<std::uint64_t> m_published{0};

    std::atomic<const Utterance*> m_live{nullptr};
    std::atomic<int> m_readers{0};
    std::vector<std::unique_ptr<Utterance>> m_retired;   // worker only
    std::unique_ptr<Utterance> m_current;                // worker only

    std::mutex m_wakeMutex;
    std::condition_variable m_wake;
    std::atomic<bool> m_stop{false};
    std::thread m_worker;
};

}  // namespace talkit
