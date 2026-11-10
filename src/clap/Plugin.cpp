// CLAP entry point. clap-wrapper turns this same binary into VST3 and AUv2,
// so there is no per-format code here.
//
// Phase 1 is the generator: text and voice settings in, one rendered phrase
// out, triggered by the transport or a note. All synthesis happens on
// RenderQueue's worker; process() only ever reads a finished buffer.

#include "../core/Params.h"
#include "../core/Player.h"
#include "../core/RenderQueue.h"
#include "Gui.h"

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace talkit {
namespace {

constexpr const char* kPluginId = "com.glebasos.talkit.generator";

const char* const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor kDescriptor = {
    CLAP_VERSION_INIT,
    kPluginId,
    "Talk It! Generator",
    "glebasos",
    "https://github.com/glebasos/TalkItVST",
    "",
    "",
    "0.1.0",
    "Speech synthesis with the 1993 SoftVoice engine, reconstructed.",
    kFeatures,
};

double clampParam(const ParamInfo& info, double v) {
    return std::clamp(v, info.min, info.max);
}

int indexOf(ParamId id) {
    for (size_t i = 0; i < kParams.size(); ++i)
        if (kParams[i].id == id) return static_cast<int>(i);
    return -1;
}

// Parameter edits made in the window have to reach the host as events, and
// the only place we may emit them is process() or params.flush() on the audio
// thread. A single-producer single-consumer ring carries them across without
// a lock or an allocation on either side. Overflow drops the oldest-unread
// edit rather than blocking the GUI; the value itself is never lost, because
// the window has already applied it locally.
class ParamRing {
public:
    struct Entry {
        clap_id id;
        double value;
    };

    bool push(clap_id id, double value) {
        const auto write = m_write.load(std::memory_order_relaxed);
        const auto next = (write + 1) % kCapacity;
        if (next == m_read.load(std::memory_order_acquire)) return false;
        m_entries[write] = Entry{id, value};
        m_write.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Entry& out) {
        const auto read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) return false;
        out = m_entries[read];
        m_read.store((read + 1) % kCapacity, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kCapacity = 256;
    std::array<Entry, kCapacity> m_entries{};
    std::atomic<size_t> m_write{0};
    std::atomic<size_t> m_read{0};
};

class Plugin final : public GuiDelegate {
public:
    explicit Plugin(const clap_host* host) : m_host(host) {
        for (size_t i = 0; i < kParams.size(); ++i) m_values[i] = kParams[i].def;
        m_queue.setText("Hello from nineteen ninety three.");
        pushVoice();
    }

    // The window's timer calls back into the queue, and members are destroyed
    // in reverse declaration order, which would take m_queue down first. Close
    // the window before anything it talks to goes away. Hosts are supposed to
    // call gui.destroy first; this does not rely on it.
    ~Plugin() override { m_gui.reset(); }

    const clap_plugin* handle() { return &m_plugin; }

    // ---- lifecycle ----------------------------------------------------
    bool init() {
        if (m_host == nullptr || m_host->get_extension == nullptr) return true;
        m_hostParams = static_cast<const clap_host_params*>(
            m_host->get_extension(m_host, CLAP_EXT_PARAMS));
        m_hostState = static_cast<const clap_host_state*>(
            m_host->get_extension(m_host, CLAP_EXT_STATE));
        return true;
    }

    bool activate(double sampleRate, std::uint32_t /*minFrames*/, std::uint32_t maxFrames) {
        m_sampleRate = sampleRate;
        m_scratch.assign(maxFrames, 0.0f);
        m_queue.setSampleRate(static_cast<int>(sampleRate));
        return true;
    }

    void reset() {
        m_player.stop();
        m_wasPlaying = false;
    }

    // ---- parameters ---------------------------------------------------
    double value(ParamId id) const {
        const int i = indexOf(id);
        return i >= 0 ? m_values[static_cast<size_t>(i)] : 0.0;
    }

    void setValue(ParamId id, double v, bool fromGui = false) {
        const int i = indexOf(id);
        if (i < 0) return;
        const auto& info = kParams[static_cast<size_t>(i)];
        const double clamped = clampParam(info, v);
        if (m_values[static_cast<size_t>(i)] == clamped) return;
        m_values[static_cast<size_t>(i)] = clamped;
        if (fromGui) m_fromGui.push(static_cast<clap_id>(id), clamped);

        // Selecting a personality loads its preset, exactly as the original
        // UI does, so the other voice controls follow it. Each derived value
        // has to be reported separately, or the host would keep automating
        // the old pitch and rate.
        if (id == ParamId::Personality) {
            const auto& preset = kPersonalities[static_cast<size_t>(clamped)];
            auto derive = [&](ParamId derived, double value) {
                m_values[static_cast<size_t>(indexOf(derived))] = value;
                if (fromGui) m_fromGui.push(static_cast<clap_id>(derived), value);
            };
            derive(ParamId::Pitch, preset.pitch);
            derive(ParamId::Rate, preset.rate);
            derive(ParamId::Voicing, static_cast<double>(preset.voicing));
            for (size_t k = 0; k < kF0StyleChoices.size(); ++k)
                if (kF0StyleChoices[k] == preset.f0Style)
                    derive(ParamId::F0Style, static_cast<double>(k));
            if (m_host && m_host->request_callback) m_host->request_callback(m_host);
        }

        if (id == ParamId::Interpolation) {
            m_queue.setInterpolation(clamped >= 0.5 ? Interpolation::Crunch
                                                    : Interpolation::Clean);
        } else if (id == ParamId::Loop) {
            m_player.setLooping(clamped >= 0.5);
        } else if (id != ParamId::Gain && id != ParamId::SyncTransport) {
            pushVoice();
        }
    }

    bool valueToText(ParamId id, double v, char* out, std::uint32_t size) const {
        auto copy = [&](std::string_view s) {
            std::snprintf(out, size, "%.*s", static_cast<int>(s.size()), s.data());
            return true;
        };
        const auto index = static_cast<size_t>(std::max(0.0, v));
        switch (id) {
            case ParamId::Personality:
                return index < kPersonalities.size() ? copy(kPersonalities[index].name) : false;
            case ParamId::Voicing:
                return index < kVoicingNames.size() ? copy(kVoicingNames[index]) : false;
            case ParamId::F0Style:
                return index < kF0StyleNames.size() ? copy(kF0StyleNames[index]) : false;
            case ParamId::Language:
                return index < kLanguageNames.size() ? copy(kLanguageNames[index]) : false;
            case ParamId::Interpolation:
                return index < kInterpolationNames.size() ? copy(kInterpolationNames[index]) : false;
            case ParamId::Loop:
            case ParamId::SyncTransport:
                return copy(v >= 0.5 ? "On" : "Off");
            case ParamId::Gain:
                std::snprintf(out, size, "%.2f", v);
                return true;
            case ParamId::Pitch:
                std::snprintf(out, size, "%d", static_cast<int>(v));
                return true;
            case ParamId::Rate:
                std::snprintf(out, size, "%d wpm", static_cast<int>(v));
                return true;
        }
        return false;
    }

    // ---- state --------------------------------------------------------
    // Plain text, one field per line: readable in a diff, and a project saved
    // by a later version still loads what it recognises.
    bool save(const clap_ostream* stream) const {
        std::string out = "talkit-state 1\n";
        for (const auto& p : kParams) {
            out += "p " + std::to_string(static_cast<std::uint32_t>(p.id)) + " " +
                   std::to_string(value(p.id)) + "\n";
        }
        const std::string text = m_queue.text();
        out += "text " + std::to_string(text.size()) + "\n";
        out += text;

        size_t written = 0;
        while (written < out.size()) {
            const auto n = stream->write(stream, out.data() + written,
                                         out.size() - written);
            if (n <= 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    }

    bool load(const clap_istream* stream) {
        std::string blob;
        char buffer[4096];
        for (;;) {
            const auto n = stream->read(stream, buffer, sizeof buffer);
            if (n < 0) return false;
            if (n == 0) break;
            blob.append(buffer, static_cast<size_t>(n));
        }
        if (blob.rfind("talkit-state ", 0) != 0) return false;

        size_t pos = blob.find('\n');
        if (pos == std::string::npos) return false;
        ++pos;

        while (pos < blob.size()) {
            const size_t eol = blob.find('\n', pos);
            const std::string line = blob.substr(pos, eol == std::string::npos
                                                         ? std::string::npos
                                                         : eol - pos);
            pos = eol == std::string::npos ? blob.size() : eol + 1;

            if (line.rfind("p ", 0) == 0) {
                unsigned id = 0;
                double v = 0;
                if (std::sscanf(line.c_str(), "p %u %lf", &id, &v) == 2) {
                    const int i = indexOf(static_cast<ParamId>(id));
                    // Written directly: going through setValue() would make
                    // Personality overwrite the saved pitch and rate.
                    if (i >= 0)
                        m_values[static_cast<size_t>(i)] =
                            clampParam(kParams[static_cast<size_t>(i)], v);
                }
            } else if (line.rfind("text ", 0) == 0) {
                size_t length = 0;
                if (std::sscanf(line.c_str(), "text %zu", &length) == 1)
                    m_queue.setText(blob.substr(pos, length));
                break;
            }
        }

        m_queue.setInterpolation(value(ParamId::Interpolation) >= 0.5 ? Interpolation::Crunch
                                                                     : Interpolation::Clean);
        m_player.setLooping(value(ParamId::Loop) >= 0.5);
        pushVoice();
        return true;
    }

    // ---- GuiDelegate --------------------------------------------------
    double paramValue(ParamId id) const override { return value(id); }

    void setParamFromGui(ParamId id, double v) override {
        setValue(id, v, true);
        markDirty();
        // Not processing (window open, transport stopped) means nothing will
        // drain the ring on its own, so ask the host for a flush.
        if (m_hostParams && m_hostParams->request_flush) m_hostParams->request_flush(m_host);
    }

    std::string text() const override { return m_queue.text(); }

    void setTextFromGui(std::string utf8) override {
        m_queue.setText(std::move(utf8));
        markDirty();
    }

    void triggerFromGui() override { m_player.trigger(); }
    void stopFromGui() override { m_player.stop(); }
    bool rendering() const override { return m_queue.rendering(); }
    std::string lastError() const override { return m_queue.lastError(); }

    double utteranceSeconds() const override {
        auto hold = const_cast<RenderQueue&>(m_queue).acquire();
        return hold ? hold->seconds() : 0.0;
    }

    // ---- gui ----------------------------------------------------------
    bool guiCreate() {
        if (!Gui::supported()) return false;
        m_gui = Gui::create(*this);
        return m_gui != nullptr;
    }
    void guiDestroy() { m_gui.reset(); }
    Gui* gui() { return m_gui.get(); }

    // ---- audio --------------------------------------------------------
    clap_process_status process(const clap_process* process) {
        const auto frames = process->frames_count;
        if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 1)
            return CLAP_PROCESS_ERROR;
        // activate() sized the scratch buffer to the host's stated maximum.
        // A bigger block than promised is the host's bug, and growing the
        // buffer here would allocate on the audio thread.
        if (frames > m_scratch.size()) return CLAP_PROCESS_ERROR;

        if (value(ParamId::SyncTransport) >= 0.5 && process->transport) {
            const bool playing =
                (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
            if (playing && !m_wasPlaying) m_player.trigger();
            if (!playing && m_wasPlaying) m_player.stop();
            m_wasPlaying = playing;
        }

        emitGuiEdits(process->out_events);

        auto hold = m_queue.acquire();
        const Utterance* utterance = hold ? &*hold : nullptr;
        const auto gain = static_cast<float>(value(ParamId::Gain));

        const auto* in = process->in_events;
        const std::uint32_t eventCount = in ? in->size(in) : 0;
        std::uint32_t eventIndex = 0;
        std::uint32_t done = 0;

        while (done < frames) {
            // Split the block at the next event so parameter and note timing
            // stays sample accurate.
            std::uint32_t next = frames;
            while (eventIndex < eventCount) {
                const clap_event_header* header = in->get(in, eventIndex);
                if (header->time > done) {
                    next = std::min(next, header->time);
                    break;
                }
                handleEvent(header);
                ++eventIndex;
            }

            const auto chunk = next - done;
            if (chunk > 0) {
                m_player.process(utterance, m_scratch.data() + done,
                                 static_cast<int>(chunk), gain);
                done = next;
            } else {
                break;
            }
        }
        if (done < frames)
            std::fill(m_scratch.begin() + done, m_scratch.begin() + frames, 0.0f);

        // Mono engine, so every output channel gets the same signal.
        auto& out = process->audio_outputs[0];
        for (std::uint32_t c = 0; c < out.channel_count; ++c) {
            if (out.data32 && out.data32[c])
                std::copy(m_scratch.begin(), m_scratch.begin() + frames, out.data32[c]);
        }

        // Keep getting called while a render is in flight, so the first buffer
        // is heard the moment it is ready.
        return (m_player.playing() || m_queue.rendering()) ? CLAP_PROCESS_CONTINUE
                                                           : CLAP_PROCESS_SLEEP;
    }

    RenderQueue& queue() { return m_queue; }

    // Drains edits made in the window out to the host. Audio thread.
    void emitGuiEdits(const clap_output_events* out) {
        if (out == nullptr || out->try_push == nullptr) return;
        ParamRing::Entry entry{};
        while (m_fromGui.pop(entry)) {
            clap_event_param_value event{};
            event.header.size = sizeof event;
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.flags = 0;
            event.param_id = entry.id;
            event.cookie = nullptr;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = entry.value;
            out->try_push(out, &event.header);
        }
    }

private:
    void markDirty() const {
        if (m_hostState && m_hostState->mark_dirty) m_hostState->mark_dirty(m_host);
    }

    void handleEvent(const clap_event_header* header) {
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        switch (header->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* e = reinterpret_cast<const clap_event_param_value*>(header);
                setValue(static_cast<ParamId>(e->param_id), e->value);
                break;
            }
            // A stopgap until the instrument phase: any note on restarts the
            // phrase, so it can be driven from a piano roll already. Pitch is
            // ignored here on purpose, because playing the note's pitch needs
            // the pre-rendered per-semitone bank, not resampling.
            case CLAP_EVENT_NOTE_ON:
                m_player.trigger();
                break;
            case CLAP_EVENT_NOTE_OFF:
            case CLAP_EVENT_NOTE_CHOKE:
                break;
            default:
                break;
        }
    }

    void pushVoice() {
        VoiceSettings v;
        v.language = value(ParamId::Language) >= 0.5 ? Language::Spanish : Language::English;
        v.personality = static_cast<int>(value(ParamId::Personality));
        v.pitch = static_cast<int>(value(ParamId::Pitch));
        v.rate = static_cast<int>(value(ParamId::Rate));
        v.voicing = static_cast<Voicing>(static_cast<int>(value(ParamId::Voicing)));
        const auto styleIndex = static_cast<size_t>(value(ParamId::F0Style));
        v.f0Style = styleIndex < kF0StyleChoices.size() ? kF0StyleChoices[styleIndex]
                                                        : F0Style::Natural;
        m_queue.setVoice(v);
    }

    const clap_host* m_host = nullptr;
    const clap_host_params* m_hostParams = nullptr;
    const clap_host_state* m_hostState = nullptr;
    clap_plugin m_plugin{};
    std::array<double, kParams.size()> m_values{};
    std::vector<float> m_scratch;
    double m_sampleRate = 44100.0;
    bool m_wasPlaying = false;

    ParamRing m_fromGui;
    std::unique_ptr<Gui> m_gui;
    RenderQueue m_queue;
    Player m_player;

    friend const clap_plugin* makePlugin(const clap_host*);
};

Plugin* self(const clap_plugin* p) { return static_cast<Plugin*>(p->plugin_data); }

// ---- extensions -------------------------------------------------------

std::uint32_t audioPortsCount(const clap_plugin*, bool isInput) { return isInput ? 0 : 1; }

bool audioPortsGet(const clap_plugin*, std::uint32_t index, bool isInput,
                   clap_audio_port_info* info) {
    if (isInput || index != 0) return false;
    info->id = 0;
    std::snprintf(info->name, sizeof info->name, "%s", "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports kAudioPorts = {audioPortsCount, audioPortsGet};

std::uint32_t notePortsCount(const clap_plugin*, bool isInput) { return isInput ? 1 : 0; }

bool notePortsGet(const clap_plugin*, std::uint32_t index, bool isInput,
                  clap_note_port_info* info) {
    if (!isInput || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof info->name, "%s", "Trigger");
    return true;
}

const clap_plugin_note_ports kNotePorts = {notePortsCount, notePortsGet};

std::uint32_t paramsCount(const clap_plugin*) {
    return static_cast<std::uint32_t>(kParams.size());
}

bool paramsGetInfo(const clap_plugin*, std::uint32_t index, clap_param_info* info) {
    if (index >= kParams.size()) return false;
    const auto& p = kParams[index];
    info->id = static_cast<clap_id>(p.id);
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (p.stepped) info->flags |= CLAP_PARAM_IS_STEPPED;
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof info->name, "%.*s",
                  static_cast<int>(p.name.size()), p.name.data());
    std::snprintf(info->module, sizeof info->module, "%.*s",
                  static_cast<int>(p.module.size()), p.module.data());
    info->min_value = p.min;
    info->max_value = p.max;
    info->default_value = p.def;
    return true;
}

bool paramsGetValue(const clap_plugin* plugin, clap_id id, double* out) {
    if (findParam(static_cast<ParamId>(id)) == nullptr) return false;
    *out = self(plugin)->value(static_cast<ParamId>(id));
    return true;
}

bool paramsValueToText(const clap_plugin* plugin, clap_id id, double value,
                       char* out, std::uint32_t size) {
    return self(plugin)->valueToText(static_cast<ParamId>(id), value, out, size);
}

bool paramsTextToValue(const clap_plugin*, clap_id id, const char* text, double* out) {
    const auto* info = findParam(static_cast<ParamId>(id));
    if (info == nullptr || text == nullptr) return false;
    try {
        *out = clampParam(*info, std::stod(text));
        return true;
    } catch (...) {
        return false;
    }
}

void paramsFlush(const clap_plugin* plugin, const clap_input_events* in,
                 const clap_output_events* out) {
    auto* p = self(plugin);
    if (in != nullptr) {
        const auto count = in->size(in);
        for (std::uint32_t i = 0; i < count; ++i) {
            const clap_event_header* header = in->get(in, i);
            if (header->space_id == CLAP_CORE_EVENT_SPACE_ID &&
                header->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* e = reinterpret_cast<const clap_event_param_value*>(header);
                p->setValue(static_cast<ParamId>(e->param_id), e->value);
            }
        }
    }
    // The other half of request_flush(): this is where window edits reach the
    // host while the transport is stopped.
    p->emitGuiEdits(out);
}

const clap_plugin_params kParamsExt = {paramsCount, paramsGetInfo, paramsGetValue,
                                       paramsValueToText, paramsTextToValue, paramsFlush};

bool stateSave(const clap_plugin* plugin, const clap_ostream* stream) {
    return self(plugin)->save(stream);
}
bool stateLoad(const clap_plugin* plugin, const clap_istream* stream) {
    return self(plugin)->load(stream);
}
const clap_plugin_state kStateExt = {stateSave, stateLoad};

// ---- gui --------------------------------------------------------------
// Fixed size and parented only: no floating window, no host-driven resize.

bool guiIsApiSupported(const clap_plugin*, const char* api, bool isFloating) {
    return !isFloating && api != nullptr && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
}

bool guiGetPreferredApi(const clap_plugin*, const char** api, bool* isFloating) {
    *api = CLAP_WINDOW_API_COCOA;
    *isFloating = false;
    return true;
}

bool guiCreate(const clap_plugin* plugin, const char* api, bool isFloating) {
    if (!guiIsApiSupported(plugin, api, isFloating)) return false;
    return self(plugin)->guiCreate();
}

void guiDestroyExt(const clap_plugin* plugin) { self(plugin)->guiDestroy(); }

bool guiSetScale(const clap_plugin*, double) { return false; }

bool guiGetSize(const clap_plugin* plugin, std::uint32_t* width, std::uint32_t* height) {
    auto* gui = self(plugin)->gui();
    if (gui == nullptr) return false;
    gui->size(*width, *height);
    return true;
}

bool guiCanResize(const clap_plugin*) { return false; }
bool guiGetResizeHints(const clap_plugin*, clap_gui_resize_hints*) { return false; }
bool guiAdjustSize(const clap_plugin* plugin, std::uint32_t* width, std::uint32_t* height) {
    return guiGetSize(plugin, width, height);
}
bool guiSetSize(const clap_plugin*, std::uint32_t, std::uint32_t) { return false; }

bool guiSetParent(const clap_plugin* plugin, const clap_window* window) {
    auto* gui = self(plugin)->gui();
    if (gui == nullptr || window == nullptr) return false;
    return gui->setParent(window->cocoa);
}

bool guiSetTransient(const clap_plugin*, const clap_window*) { return false; }
void guiSuggestTitle(const clap_plugin*, const char*) {}

bool guiShow(const clap_plugin* plugin) {
    auto* gui = self(plugin)->gui();
    if (gui == nullptr) return false;
    gui->show();
    return true;
}

bool guiHide(const clap_plugin* plugin) {
    auto* gui = self(plugin)->gui();
    if (gui == nullptr) return false;
    gui->hide();
    return true;
}

const clap_plugin_gui kGuiExt = {
    guiIsApiSupported, guiGetPreferredApi, guiCreate,   guiDestroyExt,
    guiSetScale,       guiGetSize,         guiCanResize, guiGetResizeHints,
    guiAdjustSize,     guiSetSize,         guiSetParent, guiSetTransient,
    guiSuggestTitle,   guiShow,            guiHide,
};

// ---- plugin vtable ----------------------------------------------------

bool pluginInit(const clap_plugin* plugin) { return self(plugin)->init(); }

void pluginDestroy(const clap_plugin* plugin) { delete self(plugin); }

bool pluginActivate(const clap_plugin* plugin, double sampleRate,
                    std::uint32_t minFrames, std::uint32_t maxFrames) {
    return self(plugin)->activate(sampleRate, minFrames, maxFrames);
}

void pluginDeactivate(const clap_plugin*) {}
bool pluginStartProcessing(const clap_plugin*) { return true; }
void pluginStopProcessing(const clap_plugin*) {}
void pluginReset(const clap_plugin* plugin) { self(plugin)->reset(); }

clap_process_status pluginProcess(const clap_plugin* plugin, const clap_process* process) {
    return self(plugin)->process(process);
}

const void* pluginGetExtension(const clap_plugin*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &kNotePorts;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParamsExt;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kStateExt;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0 && Gui::supported()) return &kGuiExt;
    return nullptr;
}

void pluginOnMainThread(const clap_plugin*) {}

const clap_plugin* makePlugin(const clap_host* host) {
    auto* instance = new Plugin(host);
    instance->m_plugin = clap_plugin{
        &kDescriptor, instance,    pluginInit,           pluginDestroy,
        pluginActivate, pluginDeactivate, pluginStartProcessing, pluginStopProcessing,
        pluginReset,  pluginProcess, pluginGetExtension,  pluginOnMainThread,
    };
    return instance->handle();
}

// ---- factory and entry ------------------------------------------------

std::uint32_t factoryCount(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor* factoryDescriptor(const clap_plugin_factory*,
                                                std::uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin* factoryCreate(const clap_plugin_factory*, const clap_host* host,
                                 const char* id) {
    if (host == nullptr || id == nullptr || std::strcmp(id, kPluginId) != 0) return nullptr;
    return makePlugin(host);
}

const clap_plugin_factory kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}

const void* entryGetFactory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace
}  // namespace talkit

extern "C" {
CLAP_EXPORT const clap_plugin_entry clap_entry = {
    CLAP_VERSION_INIT,
    talkit::entryInit,
    talkit::entryDeinit,
    talkit::entryGetFactory,
};
}
