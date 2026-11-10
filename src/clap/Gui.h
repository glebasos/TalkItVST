#pragma once
#include "../core/Params.h"

#include <cstdint>
#include <memory>
#include <string>

namespace talkit {

// What the view needs from the plugin. Every call is on the main thread:
// CLAP guarantees that for the whole clap_plugin_gui extension, and the view
// only ever runs from AppKit.
class GuiDelegate {
public:
    virtual ~GuiDelegate() = default;

    virtual double paramValue(ParamId) const = 0;

    // Applies the change and tells the host about it, so automation lanes and
    // the generic panel follow the window.
    virtual void setParamFromGui(ParamId, double) = 0;

    virtual std::string text() const = 0;
    virtual void setTextFromGui(std::string) = 0;

    virtual void triggerFromGui() = 0;
    virtual void stopFromGui() = 0;

    // For the status line: what the worker is doing, and why it last failed.
    virtual bool rendering() const = 0;
    virtual std::string lastError() const = 0;
    virtual double utteranceSeconds() const = 0;
};

// The plugin window. One implementation per platform; only Cocoa exists.
class Gui {
public:
    virtual ~Gui() = default;

    // Non-null only where a GUI is implemented.
    static std::unique_ptr<Gui> create(GuiDelegate&);
    static bool supported();

    // `parent` is a host-owned NSView to become a subview of.
    virtual bool setParent(void* parent) = 0;
    virtual void size(std::uint32_t& width, std::uint32_t& height) const = 0;
    virtual void show() = 0;
    virtual void hide() = 0;
};

}  // namespace talkit
