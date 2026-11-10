// The plugin window, in AppKit. Laid out after the original Talk It!: a text
// box on top, the 4x5 personality grid in the middle, its parameter groups
// below.
//
// Everything here runs on the main thread. Parameter edits go out through
// GuiDelegate, which tells the host, so automation lanes follow the window;
// a 20 Hz timer pulls values back the other way so the window follows
// automation. Text is debounced, because committing on every keystroke would
// re-render and mark the project dirty on every character.
//
// Objective-C class names are global to the host process, so two builds of
// this plugin loaded at once would collide on them. The TalkItVST_ prefix
// makes that unlikely rather than impossible; registering the classes at run
// time under a unique name is the real fix if it ever bites.

#include "Gui.h"

#include "../core/Voice.h"

#import <Cocoa/Cocoa.h>

#include <chrono>
#include <string>

namespace {

constexpr std::uint32_t kWidth = 600;
constexpr std::uint32_t kHeight = 560;

// The original's 4x5 grid. Adding a personality without growing the grid
// would silently drop it off the window, so tie the two together.
constexpr int kGridColumns = 4;
constexpr int kGridRows = 5;
static_assert(kGridColumns * kGridRows == talkit::kPersonalityCount,
              "the personality grid must hold every personality");

// Tags, so one action method can serve a whole group.
enum : NSInteger {
    kTagTalk = 1000,
    kTagStop,
    kTagLoop,
    kTagSyncTransport,
    kTagPitch,
    kTagRate,
    kTagGain,
    kTagPitchQuality,
    kTagVocalEffort,
    kTagLanguage,
    kTagResampling,
    kTagPersonalityBase = 2000,
};

// stringWithUTF8String: returns nil on invalid UTF-8, and the wrapped targets
// compile this file with clap-wrapper's -Wpedantic -Werror, which rejects the
// GNU `?:` shorthand this would otherwise use.
NSString* toNSString(const std::string& s) {
    NSString* result = [NSString stringWithUTF8String:s.c_str()];
    return result != nil ? result : @"";
}

NSTextField* makeLabel(NSString* text, NSRect frame, bool secondary = false) {
    NSTextField* label = [[NSTextField alloc] initWithFrame:frame];
    label.stringValue = text;
    label.editable = NO;
    label.selectable = NO;
    label.bordered = NO;
    label.drawsBackground = NO;
    label.font = [NSFont systemFontOfSize:secondary ? 10 : 11];
    label.textColor = secondary ? NSColor.secondaryLabelColor : NSColor.labelColor;
    return label;
}

}  // namespace

// ---------------------------------------------------------------------------

@interface TalkItVST_View : NSView {
@public
    talkit::GuiDelegate* delegate;
}
@property(nonatomic, strong) NSTextView* textView;
@property(nonatomic, strong) NSTextField* status;
@property(nonatomic, strong) NSMutableArray<NSButton*>* personalities;
@property(nonatomic, strong) NSSlider* pitch;
@property(nonatomic, strong) NSSlider* rate;
@property(nonatomic, strong) NSSlider* gain;
@property(nonatomic, strong) NSTextField* pitchValue;
@property(nonatomic, strong) NSTextField* rateValue;
@property(nonatomic, strong) NSTextField* gainValue;
@property(nonatomic, strong) NSSegmentedControl* pitchQuality;
@property(nonatomic, strong) NSSegmentedControl* vocalEffort;
@property(nonatomic, strong) NSSegmentedControl* language;
@property(nonatomic, strong) NSSegmentedControl* resampling;
@property(nonatomic, strong) NSButton* loop;
@property(nonatomic, strong) NSButton* syncTransport;
@property(nonatomic, strong) NSTimer* timer;
@property(nonatomic, assign) BOOL refreshing;
@property(nonatomic, assign) BOOL textPending;
@property(nonatomic, assign) NSTimeInterval textTouchedAt;
- (void)buildSubviews;
- (void)refresh;
// NSTimer retains its target, so the view cannot deallocate while the timer
// is live. The owner has to break that cycle explicitly.
- (void)teardown;
@end

@implementation TalkItVST_View

- (BOOL)isFlipped { return NO; }

- (void)buildSubviews {
    self.personalities = [NSMutableArray array];
    const CGFloat W = kWidth, H = kHeight;
    const CGFloat margin = 16;
    const CGFloat full = W - 2 * margin;

    // ---- text ---------------------------------------------------------
    [self addSubview:makeLabel(@"Text", NSMakeRect(margin, H - 30, 100, 16))];

    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(margin, H - 140, full, 104)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.autohidesScrollers = YES;

    self.textView = [[NSTextView alloc]
        initWithFrame:NSMakeRect(0, 0, full, 104)];
    self.textView.font = [NSFont userFixedPitchFontOfSize:12];
    self.textView.automaticQuoteSubstitutionEnabled = NO;
    self.textView.automaticDashSubstitutionEnabled = NO;
    self.textView.automaticTextReplacementEnabled = NO;
    self.textView.allowsUndo = YES;
    self.textView.delegate = (id<NSTextViewDelegate>)self;
    self.textView.minSize = NSMakeSize(0, 0);
    self.textView.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    self.textView.verticallyResizable = YES;
    self.textView.horizontallyResizable = NO;
    self.textView.textContainer.widthTracksTextView = YES;
    scroll.documentView = self.textView;
    [self addSubview:scroll];

    // ---- transport ----------------------------------------------------
    NSButton* talk = [[NSButton alloc]
        initWithFrame:NSMakeRect(margin, H - 176, 96, 30)];
    talk.title = @"Talk It!";
    talk.bezelStyle = NSBezelStyleRounded;
    talk.target = self;
    talk.action = @selector(controlChanged:);
    talk.tag = kTagTalk;
    talk.keyEquivalent = @"\r";
    [self addSubview:talk];

    NSButton* stop = [[NSButton alloc]
        initWithFrame:NSMakeRect(margin + 102, H - 176, 72, 30)];
    stop.title = @"Stop";
    stop.bezelStyle = NSBezelStyleRounded;
    stop.target = self;
    stop.action = @selector(controlChanged:);
    stop.tag = kTagStop;
    [self addSubview:stop];

    self.loop = [[NSButton alloc] initWithFrame:NSMakeRect(margin + 186, H - 172, 70, 22)];
    [self.loop setButtonType:NSButtonTypeSwitch];
    self.loop.title = @"Loop";
    self.loop.target = self;
    self.loop.action = @selector(controlChanged:);
    self.loop.tag = kTagLoop;
    [self addSubview:self.loop];

    self.syncTransport =
        [[NSButton alloc] initWithFrame:NSMakeRect(margin + 258, H - 172, 140, 22)];
    [self.syncTransport setButtonType:NSButtonTypeSwitch];
    self.syncTransport.title = @"Speak on play";
    self.syncTransport.target = self;
    self.syncTransport.action = @selector(controlChanged:);
    self.syncTransport.tag = kTagSyncTransport;
    [self addSubview:self.syncTransport];

    self.status = makeLabel(@"", NSMakeRect(margin, H - 200, full, 16), true);
    [self addSubview:self.status];

    // ---- personality grid --------------------------------------------
    [self addSubview:makeLabel(@"Personality", NSMakeRect(margin, H - 230, 120, 16))];

    const CGFloat cellW = (full - (kGridColumns - 1) * 4) / kGridColumns;
    for (int i = 0; i < talkit::kPersonalityCount; ++i) {
        const int row = i / kGridColumns;
        const int column = i % kGridColumns;
        const NSRect frame = NSMakeRect(margin + column * (cellW + 4),
                                        H - 272 - row * 26, cellW, 24);
        NSButton* button = [[NSButton alloc] initWithFrame:frame];
        [button setButtonType:NSButtonTypePushOnPushOff];
        button.bezelStyle = NSBezelStyleRounded;
        button.font = [NSFont systemFontOfSize:11];
        button.title = toNSString(std::string(talkit::kPersonalities[i].name));
        button.target = self;
        button.action = @selector(controlChanged:);
        button.tag = kTagPersonalityBase + i;
        [self addSubview:button];
        [self.personalities addObject:button];
    }

    // ---- sliders ------------------------------------------------------
    struct SliderSpec {
        NSString* name;
        NSInteger tag;
        double min, max;
        CGFloat y;
    };
    const SliderSpec specs[] = {
        {@"Pitch", kTagPitch, talkit::kPitchMin, talkit::kPitchMax, 140},
        {@"Rate", kTagRate, talkit::kRateMin, talkit::kRateMax, 112},
        {@"Gain", kTagGain, 0.0, 2.0, 84},
    };
    for (const auto& spec : specs) {
        [self addSubview:makeLabel(spec.name, NSMakeRect(margin, spec.y + 2, 52, 16))];
        NSSlider* slider =
            [[NSSlider alloc] initWithFrame:NSMakeRect(margin + 56, spec.y, full - 56 - 72, 20)];
        slider.minValue = spec.min;
        slider.maxValue = spec.max;
        slider.continuous = YES;
        slider.target = self;
        slider.action = @selector(controlChanged:);
        slider.tag = spec.tag;
        [self addSubview:slider];

        NSTextField* value =
            makeLabel(@"", NSMakeRect(W - margin - 66, spec.y + 2, 66, 16), true);
        value.alignment = NSTextAlignmentRight;
        [self addSubview:value];

        if (spec.tag == kTagPitch) { self.pitch = slider; self.pitchValue = value; }
        if (spec.tag == kTagRate)  { self.rate  = slider; self.rateValue  = value; }
        if (spec.tag == kTagGain)  { self.gain  = slider; self.gainValue  = value; }
    }

    // ---- choices ------------------------------------------------------
    auto addSegmented = [&](NSString* name, NSInteger tag,
                            const std::initializer_list<NSString*>& titles,
                            NSRect labelFrame, NSRect frame) -> NSSegmentedControl* {
        [self addSubview:makeLabel(name, labelFrame)];
        NSSegmentedControl* segmented = [[NSSegmentedControl alloc] initWithFrame:frame];
        segmented.segmentCount = static_cast<NSInteger>(titles.size());
        NSInteger index = 0;
        for (NSString* title : titles) [segmented setLabel:title forSegment:index++];
        segmented.segmentStyle = NSSegmentStyleRounded;
        segmented.font = [NSFont systemFontOfSize:11];
        segmented.target = self;
        segmented.action = @selector(controlChanged:);
        segmented.tag = tag;
        [self addSubview:segmented];
        return segmented;
    };

    self.pitchQuality = addSegmented(@"Pitch quality", kTagPitchQuality,
                                     {@"Natural", @"Monotone", @"Sung"},
                                     NSMakeRect(margin, 58, 86, 16),
                                     NSMakeRect(margin + 90, 52, 186, 24));
    self.vocalEffort = addSegmented(@"Effort", kTagVocalEffort,
                                    {@"Normal", @"Breathy", @"Whispered"},
                                    NSMakeRect(margin + 288, 58, 44, 16),
                                    NSMakeRect(margin + 336, 52, 216, 24));
    self.language = addSegmented(@"Language", kTagLanguage, {@"English", @"Spanish"},
                                 NSMakeRect(margin, 22, 86, 16),
                                 NSMakeRect(margin + 90, 16, 186, 24));
    self.resampling = addSegmented(@"Resampling", kTagResampling, {@"Clean", @"Crunch"},
                                   NSMakeRect(margin + 284, 22, 80, 16),
                                   NSMakeRect(margin + 368, 16, 184, 24));

    if (delegate) {
        self.textView.string = toNSString(delegate->text());
    }
    [self refresh];

    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.05
                                                  target:self
                                                selector:@selector(onTimer)
                                                userInfo:nil
                                                 repeats:YES];
}

- (void)teardown {
    [self.timer invalidate];
    self.timer = nil;
    self.textView.delegate = nil;
    delegate = nullptr;
}

- (void)dealloc { [self teardown]; }

// ---- the window follows the host ----------------------------------------

- (void)refresh {
    if (delegate == nullptr) return;
    self.refreshing = YES;

    using talkit::ParamId;
    const auto personality = static_cast<int>(delegate->paramValue(ParamId::Personality));
    for (int i = 0; i < static_cast<int>(self.personalities.count); ++i)
        self.personalities[static_cast<NSUInteger>(i)].state =
            (i == personality) ? NSControlStateValueOn : NSControlStateValueOff;

    const double pitch = delegate->paramValue(ParamId::Pitch);
    const double rate = delegate->paramValue(ParamId::Rate);
    const double gain = delegate->paramValue(ParamId::Gain);
    self.pitch.doubleValue = pitch;
    self.rate.doubleValue = rate;
    self.gain.doubleValue = gain;
    // Pitch is in hertz, measured one-for-one against the engine, so say so.
    self.pitchValue.stringValue = [NSString stringWithFormat:@"%d Hz", static_cast<int>(pitch)];
    self.rateValue.stringValue = [NSString stringWithFormat:@"%d wpm", static_cast<int>(rate)];
    self.gainValue.stringValue = [NSString stringWithFormat:@"%.2f", gain];

    self.pitchQuality.selectedSegment =
        static_cast<NSInteger>(delegate->paramValue(ParamId::F0Style));
    self.vocalEffort.selectedSegment =
        static_cast<NSInteger>(delegate->paramValue(ParamId::Voicing));
    self.language.selectedSegment =
        static_cast<NSInteger>(delegate->paramValue(ParamId::Language));
    self.resampling.selectedSegment =
        static_cast<NSInteger>(delegate->paramValue(ParamId::Interpolation));
    self.loop.state = delegate->paramValue(ParamId::Loop) >= 0.5 ? NSControlStateValueOn
                                                                 : NSControlStateValueOff;
    self.syncTransport.state = delegate->paramValue(ParamId::SyncTransport) >= 0.5
                                   ? NSControlStateValueOn
                                   : NSControlStateValueOff;

    const std::string error = delegate->lastError();
    if (!error.empty()) {
        self.status.stringValue = toNSString(error);
        self.status.textColor = NSColor.systemRedColor;
    } else if (delegate->rendering()) {
        self.status.stringValue = @"rendering...";
        self.status.textColor = NSColor.secondaryLabelColor;
    } else {
        const double seconds = delegate->utteranceSeconds();
        self.status.stringValue =
            seconds > 0.0 ? [NSString stringWithFormat:@"%.2f s ready", seconds] : @"";
        self.status.textColor = NSColor.secondaryLabelColor;
    }

    self.refreshing = NO;
}

- (void)onTimer {
    // Commit the text once typing pauses, rather than on every keystroke.
    if (self.textPending &&
        NSDate.timeIntervalSinceReferenceDate - self.textTouchedAt > 0.35) {
        self.textPending = NO;
        if (delegate)
            delegate->setTextFromGui(self.textView.string.UTF8String
                                         ? self.textView.string.UTF8String
                                         : "");
    }
    [self refresh];
}

- (void)textDidChange:(NSNotification*)notification {
    self.textPending = YES;
    self.textTouchedAt = NSDate.timeIntervalSinceReferenceDate;
}

// ---- the host follows the window ----------------------------------------

- (void)controlChanged:(id)sender {
    if (delegate == nullptr || self.refreshing) return;
    using talkit::ParamId;

    const NSInteger tag = [sender tag];
    if (tag >= kTagPersonalityBase) {
        delegate->setParamFromGui(ParamId::Personality,
                                  static_cast<double>(tag - kTagPersonalityBase));
        [self refresh];   // the preset moves pitch, rate and the two styles
        return;
    }

    switch (tag) {
        case kTagTalk: delegate->triggerFromGui(); break;
        case kTagStop: delegate->stopFromGui(); break;
        case kTagLoop:
            delegate->setParamFromGui(ParamId::Loop,
                                      [sender state] == NSControlStateValueOn ? 1.0 : 0.0);
            break;
        case kTagSyncTransport:
            delegate->setParamFromGui(ParamId::SyncTransport,
                                      [sender state] == NSControlStateValueOn ? 1.0 : 0.0);
            break;
        case kTagPitch:
            delegate->setParamFromGui(ParamId::Pitch, std::round([sender doubleValue]));
            break;
        case kTagRate:
            delegate->setParamFromGui(ParamId::Rate, std::round([sender doubleValue]));
            break;
        case kTagGain:
            delegate->setParamFromGui(ParamId::Gain, [sender doubleValue]);
            break;
        case kTagPitchQuality:
            delegate->setParamFromGui(
                ParamId::F0Style, static_cast<double>([sender selectedSegment]));
            break;
        case kTagVocalEffort:
            delegate->setParamFromGui(
                ParamId::Voicing, static_cast<double>([sender selectedSegment]));
            break;
        case kTagLanguage:
            delegate->setParamFromGui(
                ParamId::Language, static_cast<double>([sender selectedSegment]));
            break;
        case kTagResampling:
            delegate->setParamFromGui(
                ParamId::Interpolation, static_cast<double>([sender selectedSegment]));
            break;
        default: break;
    }
    [self refresh];
}

@end

// ---------------------------------------------------------------------------

namespace talkit {
namespace {

class CocoaGui final : public Gui {
public:
    explicit CocoaGui(GuiDelegate& delegate) {
        TalkItVST_View* view = [[TalkItVST_View alloc]
            initWithFrame:NSMakeRect(0, 0, kWidth, kHeight)];
        view->delegate = &delegate;
        [view buildSubviews];
        m_view = (__bridge_retained void*)view;
    }

    ~CocoaGui() override {
        TalkItVST_View* view = (__bridge_transfer TalkItVST_View*)m_view;
        m_view = nullptr;
        // teardown first: it drops the timer's reference to the view and stops
        // any pending fire from reaching a delegate that is going away.
        [view teardown];
        [view removeFromSuperview];
    }

    bool setParent(void* parent) override {
        if (parent == nullptr || m_view == nullptr) return false;
        NSView* host = (__bridge NSView*)parent;
        TalkItVST_View* view = (__bridge TalkItVST_View*)m_view;
        [view removeFromSuperview];
        view.frame = NSMakeRect(0, 0, kWidth, kHeight);
        [host addSubview:view];
        return true;
    }

    void size(std::uint32_t& width, std::uint32_t& height) const override {
        width = kWidth;
        height = kHeight;
    }

    void show() override {
        if (m_view) ((__bridge TalkItVST_View*)m_view).hidden = NO;
    }

    void hide() override {
        if (m_view) ((__bridge TalkItVST_View*)m_view).hidden = YES;
    }

private:
    void* m_view = nullptr;
};

}  // namespace

bool Gui::supported() { return true; }

std::unique_ptr<Gui> Gui::create(GuiDelegate& delegate) {
    return std::make_unique<CocoaGui>(delegate);
}

}  // namespace talkit
