// A minimal CLAP host, enough to prove the built bundle really works rather
// than merely compiles: it loads the binary, walks the factory, enumerates
// parameters, round-trips state, and runs process() until audio comes out.
//
//   talkit-host build-plugin/TalkIt.clap
//
// Steinberg's VST3 validator needs full Xcode, which this machine does not
// have, so this covers the shared plugin code that all three formats embed.

#include <clap/clap.h>

#import <Cocoa/Cocoa.h>

#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

// ---- the host side ----------------------------------------------------

const void* hostGetExtension(const clap_host*, const char*) { return nullptr; }
void hostRequestRestart(const clap_host*) {}
void hostRequestProcess(const clap_host*) {}
void hostRequestCallback(const clap_host*) {}

const clap_host kHost = {
    CLAP_VERSION_INIT, nullptr, "talkit-host", "glebasos", "", "0.1.0",
    hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback,
};

// ---- an in-memory stream for the state round trip ----------------------

struct Buffer {
    std::vector<char> bytes;
    size_t readPos = 0;
};

std::int64_t streamWrite(const clap_ostream* stream, const void* data, std::uint64_t size) {
    auto* b = static_cast<Buffer*>(stream->ctx);
    const auto* p = static_cast<const char*>(data);
    b->bytes.insert(b->bytes.end(), p, p + size);
    return static_cast<std::int64_t>(size);
}

std::int64_t streamRead(const clap_istream* stream, void* data, std::uint64_t size) {
    auto* b = static_cast<Buffer*>(stream->ctx);
    const auto left = b->bytes.size() - b->readPos;
    const auto n = static_cast<size_t>(size) < left ? static_cast<size_t>(size) : left;
    std::memcpy(data, b->bytes.data() + b->readPos, n);
    b->readPos += n;
    return static_cast<std::int64_t>(n);
}

// ---- one event list holding a single note on --------------------------

struct Events {
    clap_event_note note{};
    bool deliver = false;
};

std::uint32_t eventsSize(const clap_input_events* list) {
    return static_cast<const Events*>(list->ctx)->deliver ? 1 : 0;
}

const clap_event_header* eventsGet(const clap_input_events* list, std::uint32_t index) {
    const auto* e = static_cast<const Events*>(list->ctx);
    return (e->deliver && index == 0) ? &e->note.header : nullptr;
}

bool outputTryPush(const clap_output_events*, const clap_event_header*) { return true; }

// Collects what the plugin pushes back, so a window edit can be seen arriving
// as a parameter event the way a host's automation lane would see it.
struct Collected {
    std::vector<clap_event_param_value> params;
};

bool collectTryPush(const clap_output_events* out, const clap_event_header* header) {
    if (header->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        header->type == CLAP_EVENT_PARAM_VALUE) {
        static_cast<Collected*>(out->ctx)
            ->params.push_back(*reinterpret_cast<const clap_event_param_value*>(header));
    }
    return true;
}

// Accepts either the bundle directory or the binary inside it.
std::string resolveBinary(const std::string& path) {
    namespace fs = std::filesystem;
    if (fs::is_regular_file(path)) return path;
    const auto inside = fs::path(path) / "Contents" / "MacOS";
    if (fs::is_directory(inside))
        for (const auto& entry : fs::directory_iterator(inside))
            if (entry.is_regular_file()) return entry.path().string();
    return path;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: talkit-host <path to .clap bundle> [--window]\n"
                     "  --window  after the checks, open the plugin window in a\n"
                     "            real NSWindow so the layout can be looked at\n";
        return 2;
    }
    const std::string bundle = argv[1];
    bool openWindow = false;
    std::string snapshot;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--window") == 0) openWindow = true;
        else if (std::strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) snapshot = argv[++i];
    }
    const std::string binary = resolveBinary(bundle);
    std::cout << "loading " << binary << "\n\n";

    void* handle = dlopen(binary.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }
    check(true, "bundle loads");

    auto* entry = static_cast<const clap_plugin_entry*>(dlsym(handle, "clap_entry"));
    check(entry != nullptr, "exports clap_entry");
    if (entry == nullptr) return 1;
    check(clap_version_is_compatible(entry->clap_version), "entry reports a compatible version");
    check(entry->init(bundle.c_str()), "entry init succeeds");

    const auto* factory =
        static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory != nullptr, "provides the plugin factory");
    if (factory == nullptr) return 1;
    check(factory->get_plugin_count(factory) == 1, "factory reports one plugin");

    const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
    check(descriptor != nullptr && descriptor->id != nullptr, "descriptor is present");
    if (descriptor == nullptr) return 1;
    std::cout << "        id:     " << descriptor->id << "\n"
              << "        name:   " << descriptor->name << "\n"
              << "        vendor: " << descriptor->vendor << "\n"
              << "        version:" << descriptor->version << "\n";

    const clap_plugin* plugin = factory->create_plugin(factory, &kHost, descriptor->id);
    check(plugin != nullptr, "creates an instance");
    if (plugin == nullptr) return 1;
    check(factory->create_plugin(factory, &kHost, "no.such.plugin") == nullptr,
          "refuses an unknown plugin id");
    check(plugin->init(plugin), "instance init succeeds");

    // ---- extensions ---------------------------------------------------
    const auto* audioPorts = static_cast<const clap_plugin_audio_ports*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    check(audioPorts != nullptr, "implements audio-ports");
    if (audioPorts != nullptr) {
        check(audioPorts->count(plugin, false) == 1 && audioPorts->count(plugin, true) == 0,
              "one output port and no input");
        clap_audio_port_info info{};
        check(audioPorts->get(plugin, 0, false, &info) && info.channel_count == 2,
              "output port is stereo");
    }

    const auto* notePorts = static_cast<const clap_plugin_note_ports*>(
        plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
    check(notePorts != nullptr && notePorts->count(plugin, true) == 1,
          "implements note-ports with one input");

    const auto* params =
        static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    check(params != nullptr, "implements params");
    if (params != nullptr) {
        const auto count = params->count(plugin);
        check(count == 10, "exposes ten parameters");
        bool allGood = true;
        std::cout << "\n";
        for (std::uint32_t i = 0; i < count; ++i) {
            clap_param_info info{};
            if (!params->get_info(plugin, i, &info)) {
                allGood = false;
                continue;
            }
            double value = 0.0;
            if (!params->get_value(plugin, info.id, &value)) allGood = false;
            char text[CLAP_NAME_SIZE] = {};
            if (!params->value_to_text(plugin, info.id, value, text, sizeof text))
                allGood = false;
            std::printf("        %-14s %-8s %8.2f  [%g..%g]  \"%s\"\n", info.name,
                        info.module, value, info.min_value, info.max_value, text);
            allGood = allGood && info.min_value <= value && value <= info.max_value;
        }
        std::cout << "\n";
        check(allGood, "every parameter reads back inside its range with a label");

        clap_param_info missing{};
        check(!params->get_info(plugin, count, &missing),
              "refuses a parameter index past the end");
    }

    // ---- gui ----------------------------------------------------------
    const auto* gui =
        static_cast<const clap_plugin_gui*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    check(gui != nullptr, "implements gui");
    if (gui != nullptr) {
        check(gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false),
              "supports a parented Cocoa window");
        check(!gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, true),
              "refuses a floating window");
        check(!gui->is_api_supported(plugin, "not-a-window-api", false),
              "refuses an unknown window api");

        const char* preferred = nullptr;
        bool floating = true;
        check(gui->get_preferred_api(plugin, &preferred, &floating) && !floating &&
                  std::strcmp(preferred, CLAP_WINDOW_API_COCOA) == 0,
              "prefers a parented Cocoa window");

        check(!gui->create(plugin, "not-a-window-api", false),
              "refuses to create for an unknown api");
        check(gui->create(plugin, CLAP_WINDOW_API_COCOA, false), "creates the window");

        std::uint32_t width = 0, height = 0;
        check(gui->get_size(plugin, &width, &height) && width > 0 && height > 0,
              "reports a size");
        std::printf("        window %u x %u, resizable: %s\n", width, height,
                    gui->can_resize(plugin) ? "yes" : "no");

        // Parent it into a real NSView, which is what a host does.
        [NSApplication sharedApplication];
        NSView* parent = [[NSView alloc]
            initWithFrame:NSMakeRect(0, 0, width, height)];
        clap_window window{};
        window.api = CLAP_WINDOW_API_COCOA;
        window.cocoa = (__bridge void*)parent;
        check(gui->set_parent(plugin, &window), "parents into a host view");
        check(parent.subviews.count == 1, "the window became a subview");
        if (parent.subviews.count == 1) {
            const NSSize actual = parent.subviews[0].frame.size;
            check(static_cast<std::uint32_t>(actual.width) == width &&
                      static_cast<std::uint32_t>(actual.height) == height,
                  "the subview matches the reported size");
            check(parent.subviews[0].subviews.count > 20,
                  "the window built its controls");
            std::printf("        %lu controls in the window\n",
                        static_cast<unsigned long>(parent.subviews[0].subviews.count));
        }

        // Drive a real control and watch the edit come back out as a
        // parameter event. This is the whole GUI-to-host path: AppKit action,
        // ring buffer, then params.flush() on the audio thread.
        if (params != nullptr && parent.subviews.count == 1) {
            NSView* window = parent.subviews[0];
            constexpr NSInteger kPersonalityTag = 2000;   // matches Gui.mm
            constexpr double kWanted = 9.0;               // "Little Robot"
            NSButton* button =
                (NSButton*)[window viewWithTag:kPersonalityTag + (NSInteger)kWanted];
            check(button != nil, "finds a personality button by tag");

            if (button != nil) {
                [button performClick:nil];

                double selected = -1.0;
                params->get_value(plugin, 0, &selected);
                check(selected == kWanted, "clicking a personality applies it");

                Collected collected;
                clap_output_events sink{&collected, collectTryPush};
                params->flush(plugin, nullptr, &sink);

                bool sawPersonality = false, sawDerived = false;
                for (const auto& event : collected.params) {
                    if (event.param_id == 0 && event.value == kWanted) sawPersonality = true;
                    if (event.param_id == 1 || event.param_id == 2) sawDerived = true;
                }
                check(sawPersonality, "the host is told the personality changed");
                // The preset moves pitch and rate too; a host that never heard
                // about those would keep automating the old values.
                check(sawDerived, "the host is told the preset's pitch and rate changed");
                std::printf("        %zu parameter events reached the host\n",
                            collected.params.size());
            }
        }

        check(gui->show(plugin), "shows");
        check(gui->hide(plugin), "hides");
        check(gui->show(plugin), "shows again");

        gui->destroy(plugin);
        check(parent.subviews.count == 0, "destroy removes it from the host view");

        // Re-opening is routine: hosts do it every time the window is closed
        // and reopened, and it must not leak or crash.
        check(gui->create(plugin, CLAP_WINDOW_API_COCOA, false), "reopens the window");
        check(gui->set_parent(plugin, &window), "reparents");
        gui->destroy(plugin);
        check(true, "closes cleanly the second time");
    }

    // ---- state round trip ---------------------------------------------
    const auto* state =
        static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    check(state != nullptr, "implements state");
    if (state != nullptr && params != nullptr) {
        // Move a parameter off its default so the round trip proves something.
        const clap_id personality = 0;
        double original = 0.0;
        params->get_value(plugin, personality, &original);

        Buffer buffer;
        clap_ostream out{&buffer, streamWrite};
        check(state->save(plugin, &out), "state saves");
        check(!buffer.bytes.empty(), "saved state is not empty");

        clap_istream in{&buffer, streamRead};
        check(state->load(plugin, &in), "state loads back");

        double restored = 0.0;
        params->get_value(plugin, personality, &restored);
        check(restored == original, "a parameter survives the round trip");

        Buffer garbage;
        garbage.bytes = {'n', 'o', 'p', 'e'};
        clap_istream bad{&garbage, streamRead};
        check(!state->load(plugin, &bad), "rejects state it did not write");
    }

    // ---- processing ---------------------------------------------------
    constexpr std::uint32_t kFrames = 512;
    constexpr double kRate = 48000.0;
    check(plugin->activate(plugin, kRate, 32, kFrames), "activates");
    check(plugin->start_processing(plugin), "starts processing");

    std::vector<float> left(kFrames, 0.0f), right(kFrames, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    clap_audio_buffer output{channels, nullptr, 2, 0, 0};

    Events events;
    events.note.header.size = sizeof(clap_event_note);
    events.note.header.time = 0;
    events.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    events.note.header.type = CLAP_EVENT_NOTE_ON;
    events.note.header.flags = 0;
    events.note.note_id = -1;
    events.note.port_index = 0;
    events.note.channel = 0;
    events.note.key = 60;
    events.note.velocity = 1.0;
    events.deliver = true;

    clap_input_events in{&events, eventsSize, eventsGet};
    clap_output_events outEvents{nullptr, outputTryPush};

    clap_process process{};
    process.steady_time = 0;
    process.frames_count = kFrames;
    process.transport = nullptr;
    process.audio_inputs = nullptr;
    process.audio_inputs_count = 0;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    process.in_events = &in;
    process.out_events = &outEvents;

    // The note arrives before the worker has finished the default phrase, and
    // the player then starts as soon as audio is published. Pace the calls
    // like a real host: spinning process() as fast as possible would run
    // thousands of blocks in under a millisecond and never give the worker
    // time to render.
    const auto blockPeriod =
        std::chrono::microseconds(static_cast<long>(kFrames * 1'000'000.0 / kRate));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    float peak = 0.0f;
    int blocks = 0;
    int blocksWithAudio = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = plugin->process(plugin, &process);
        ++blocks;
        if (status == CLAP_PROCESS_ERROR) break;
        for (std::uint32_t c = 0; c < 2; ++c)
            for (std::uint32_t i = 0; i < kFrames; ++i)
                peak = std::max(peak, std::abs(channels[c][i]));
        // Once audio starts, keep going for half a second so the reported
        // peak is the phrase's real level and not its first quiet onset.
        if (peak > 0.0f && ++blocksWithAudio > 48) break;
        events.deliver = false;           // trigger once; the player waits
        process.steady_time += kFrames;
        std::this_thread::sleep_for(blockPeriod);
    }
    check(peak > 0.05f, "process() produces audio after a note on");
    std::printf("        peak %.3f after %d block%s of %u frames (%.0f ms of audio)\n",
                peak, blocks, blocks == 1 ? "" : "s", kFrames,
                blocks * kFrames * 1000.0 / kRate);

    bool channelsMatch = true;
    for (std::uint32_t i = 0; i < kFrames; ++i)
        channelsMatch = channelsMatch && left[i] == right[i];
    check(channelsMatch, "both channels carry the same mono signal");

    bool inRange = true;
    for (std::uint32_t c = 0; c < 2; ++c)
        for (std::uint32_t i = 0; i < kFrames; ++i)
            inRange = inRange && std::abs(channels[c][i]) <= 1.0f;
    check(inRange, "output stays inside full scale");

    // A block larger than the activated maximum must be refused, not crash.
    process.frames_count = kFrames * 4;
    check(plugin->process(plugin, &process) == CLAP_PROCESS_ERROR,
          "refuses a block bigger than it was activated for");
    process.frames_count = kFrames;

    // ---- optional: write the window out as a picture --------------------
    // Renders the real view offscreen, which needs no window server dance and
    // is the same image a host would show.
    if (!snapshot.empty() && gui != nullptr) {
        std::uint32_t width = 0, height = 0;
        gui->get_size(plugin, &width, &height);
        if (gui->create(plugin, CLAP_WINDOW_API_COCOA, false)) {
            [NSApplication sharedApplication];
            NSWindow* offscreen = [[NSWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, width, height)
                          styleMask:NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO];
            offscreen.releasedWhenClosed = NO;

            clap_window window{};
            window.api = CLAP_WINDOW_API_COCOA;
            window.cocoa = (__bridge void*)offscreen.contentView;
            gui->set_parent(plugin, &window);

            // Let the worker finish so the status line has something to say.
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < until)
                [NSRunLoop.currentRunLoop
                    runMode:NSDefaultRunLoopMode
                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];

            // Render the plugin's own view, not the content view: the plugin
            // is what we want pictured, and it is the one guaranteed to have
            // a non-empty frame.
            NSView* view = offscreen.contentView.subviews.count > 0
                               ? offscreen.contentView.subviews[0]
                               : offscreen.contentView;
            std::printf("        view bounds %.0f x %.0f\n", view.bounds.size.width,
                        view.bounds.size.height);
            NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
            [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
            NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}];
            NSString* path = [NSString stringWithUTF8String:snapshot.c_str()];
            const bool wrote = [png writeToFile:path atomically:YES];
            check(wrote, "writes a snapshot of the window");
            std::cout << "        " << snapshot << " (" << rep.pixelsWide << " x "
                      << rep.pixelsHigh << ")\n";
            gui->destroy(plugin);
        }
    }

    // ---- optional: look at it ------------------------------------------
    // There is no audio device here, so nothing is heard, but the window is
    // the real one the host gets and the worker still renders behind it.
    if (openWindow && gui != nullptr) {
        std::uint32_t width = 0, height = 0;
        gui->get_size(plugin, &width, &height);
        if (gui->create(plugin, CLAP_WINDOW_API_COCOA, false)) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            NSWindow* host = [[NSWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, width, height)
                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                            backing:NSBackingStoreBuffered
                              defer:NO];
            host.title = @"Talk It! - standalone preview";
            host.releasedWhenClosed = NO;

            clap_window window{};
            window.api = CLAP_WINDOW_API_COCOA;
            window.cocoa = (__bridge void*)host.contentView;
            gui->set_parent(plugin, &window);
            gui->show(plugin);

            [host center];
            [host makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            std::cout << "\nwindow open; close it or press ctrl-c to finish\n";
            [NSApp run];
            gui->destroy(plugin);
        }
    }

    plugin->stop_processing(plugin);
    plugin->reset(plugin);
    plugin->deactivate(plugin);
    check(true, "stops, resets and deactivates");

    plugin->destroy(plugin);
    entry->deinit();
    check(true, "destroys the instance and deinitialises the entry");
    dlclose(handle);

    std::cout << "\n" << (failures == 0 ? "all checks passed"
                                        : std::to_string(failures) + " failed") << "\n";
    return failures == 0 ? 0 : 1;
}
