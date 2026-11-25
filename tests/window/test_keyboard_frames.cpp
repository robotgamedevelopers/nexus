// Unit test of the first switch from the Dashboard to an AI's tab as the iOS window draws it (FrameHost::buildAbove,
// main_ios.cpp): the window's frame code (frame_host.h) over a kit with no window, the server's AIs through a fake link,
// and the on-screen keyboard as Prax's UIKit backend gives it. The backend raises the keyboard while the last frame
// edited text and lowers it when it stopped (app_uikit.mm deliverFrame, uikit_view.mm setEditing), and the keyboard's
// rect reaches the frames later, from the system's notification. Here the rect comes the frame after the keyboard rose,
// a frame early (in the frame that switches), two frames late, with a frame's gap in it, or never (a hardware
// keyboard), as a full keyboard or a hardware keyboard's shortcut bar, and the device is turned halfway through. Each
// case is a fresh window: its first AI tab and the keyboard's first rise.
//
// Nothing may move once the switch is done: the focus and the keyboard each change at most once, and after they
// settle no frame asks for another. In every frame with a rect, the prompt's foot is at or above the keyboard's top. The window built straight into its root while no keyboard showed and inside a box
// above the keyboard while one did, so every node was new each time the rect came or went: the prompt took the focus
// anew, and with the rect a frame behind, the keyboard went up and down every frame.
//
// No clock (the frames' time is made up), no process, no socket, no network.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_panel.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"

#include "frame_host.h"
#include "keyboard_room.h"
#include "model.h"
#include "test_link_port.h"
#include "view.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

constexpr float kKeyboard = 320;    // the on-screen keyboard's height over the window's foot, points

// When the keyboard's rect reaches the frames, against the frame after the one that first edited text.
struct Arrival {
    const char* name;
    int delay;          // frames after the keyboard rose that the rect comes; -1: in the frame that switches
    bool gap;           // the rect is missing for one frame after it came
    bool never;         // no rect at all: a hardware keyboard
    float height = kKeyboard;   // the keyboard's height over the window's foot: a full keyboard, or a shortcut bar
};

// The keyboard's height in a window of that extent: as on a device, never more than half the window (an iPhone turned
// to landscape has a keyboard about half its height).
float keyboardHeight(const Arrival& a, PrVec2 extent) {
    return std::min(a.height, extent.y / 2);
}

struct Window {
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    Look look;
    Model model{std::vector<AIInfo>{}, true};
    View view;
    FrameHost host;
    test::FakeLink link;
    double time = 1000;
    PrVec2 extent;
    int frameNo = 0;
    bool editing = false;       // the last frame edited text: the backend's keyboard is up
    int rose = -1;              // the frame after the one that first edited text, while editing; -1: down
    bool wantsFrame = false;    // the last frame asked for another

    explicit Window(PrVec2 size) : extent(size) {
        prFontSetSetSystem(fonts, system);
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
        PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 2};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        PrKitTheme theme = windowTheme(fonts, false);
        kit = prKitCreate(ui, &theme);
        view.model = &model;
        view.look = &look;
        view.extent = extent;
        host.model = &model;
        host.view = &view;
        host.ui = ui;
        host.connect(&link, "server.test");
        link.ais({"rowan", "stella"});
    }

    ~Window() {
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prFontSystemDestroy(system);
    }

    // The keyboard's rect for the coming frame, as the frame reads it (main_ios.cpp beginFrame): empty while none
    // has reached the frames.
    PrRect keyboardRect(const Arrival& a, bool switching) const {
        const PrRect shown{0, extent.y - keyboardHeight(a, extent), extent.x, extent.y};
        if (a.never) return {};
        if (a.delay < 0 && switching) return shown;
        if (rose < 0) return {};
        const int since = frameNo - rose;
        const int from = a.delay < 0 ? 0 : a.delay;
        if (since < from) return {};
        if (a.gap && since == from + 1) return {};
        return shown;
    }

    // One frame as the iOS window makes it (main_ios.cpp): the host's frame start, then the rounds, each built above
    // the keyboard (FrameHost::buildAbove); then the backend raises or lowers the keyboard by what the frame edited.
    PrRect rect{};              // the keyboard's rect in the last frame; empty for none

    void frame(const Arrival& a, bool switching = false) {
        time += 1.0 / 120;
        rect = keyboardRect(a, switching);
        const float room = roomAboveKeyboard(extent, PrAppInsets{}, rect);
        host.beginFrame(kit, fonts, time, prAppDisplayDefault());
        PrUiFrameDesc desc{.time = time, .delta = 1.0 / 120, .extent = extent, .maxRounds = 4,
            .display = prAppDisplayDefault(), .preferences = {}};
        struct Round { Window* w; float room; } round{this, room};
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Round& r = *static_cast<Round*>(context);
            Window& w = *r.w;
            const PrLayoutNode root = prKitPanel(w.kit, kPrLayoutNodeNone, {}, PrKitPanelStyle{
                .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100),
                    .height = prKitPercent(100)}},
                .background = PrKitBackground(PrKitBackground_App)});
            w.host.buildAbove(w.kit, root, w.extent, r.room);
        }, &round);
        const PrAppOutput out = prUiOutput(ui);
        ++frameNo;
        if (out.hasTextInput && !editing) rose = frameNo;
        if (!out.hasTextInput) rose = -1;
        editing = out.hasTextInput;
        wantsFrame = out.wantsFrame;
    }

    // The prompt's place in the window, empty if it has none.
    PrRect promptBounds() const {
        if (view.prompt == kPrLayoutNodeNone) return {};
        return prLayoutRectToPx(prLayoutNodeBounds(tree, view.prompt, PrLayoutCoordinates_Viewport));
    }

    std::size_t tabOf(const std::string& name) const {
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            if (model.tab(i).kind == TabKind::AI && model.tab(i).name == name) return i;
        }
        return model.tab_count();
    }
};

// A fresh window shown on the Dashboard, then switched to an AI's tab, whose prompt takes the focus, and drawn for 300
// frames with no input.
void firstSwitch(PrVec2 size, const char* device, const Arrival& a) {
    Window w(size);
    for (int i = 0; i < 10; ++i) w.frame(a);
    CHECK(w.model.tab_count() > 0 && w.model.tab(w.model.active()).kind == TabKind::Sprint);
    CHECK(!w.editing);                      // the Dashboard edits no text: the keyboard is down
    const std::size_t rowan = w.tabOf("rowan");
    CHECK(rowan < w.model.tab_count());
    if (rowan >= w.model.tab_count()) return;
    w.model.select_tab(rowan);
    PrLayoutNode focus = prUiFocus(w.ui);
    bool editing = w.editing;
    int refocused = 0, toggled = 0, askedLate = 0, covered = 0;
    for (int i = 0; i < 300; ++i) {
        if (i == 150) {
            // The device turned: the window's extent swaps, and its next frames settle the same way.
            w.extent = PrVec2{w.extent.y, w.extent.x};
        }
        w.frame(a, i == 0);
        const PrRect prompt = w.promptBounds();
        const bool shows = w.rect.bottom > w.rect.top;
        if (shows && prompt.bottom > prompt.top && prompt.bottom > w.rect.top) ++covered;
        refocused += prUiFocus(w.ui) != focus;
        toggled += w.editing != editing;
        focus = prUiFocus(w.ui);
        editing = w.editing;
        if ((i >= 30 && i < 150) || i >= 180) askedLate += w.wantsFrame;
    }
    if (refocused > 1 || toggled > 1 || askedLate != 0 || covered != 0) {
        std::fprintf(stderr, "%s, rect %s: of 300 frames, the focus changed in %d, the keyboard in %d, %d of the "
            "settled ones asked for another, and the keyboard covered the prompt in %d\n", device, a.name, refocused,
            toggled, askedLate, covered);
    }
    CHECK(refocused <= 1);
    CHECK(toggled <= 1);
    CHECK(askedLate == 0);
    CHECK(covered == 0);
    CHECK(w.editing);                       // the prompt has the focus: the keyboard is up
    CHECK(focus != kPrLayoutNodeNone && focus == w.view.prompt);
    const float shown = a.never ? w.extent.y : w.extent.y - keyboardHeight(a, w.extent);
    CHECK(w.view.extent.y == shown);        // the window stands above the keyboard once its rect came
}

} // namespace

int main() {
    const Arrival arrivals[] = {
        {"the next frame", 0, false, false},
        {"a frame early", -1, false, false},
        {"a frame late", 1, false, false},
        {"two frames late", 2, false, false},
        {"with a frame's gap", 0, true, false},
        {"late with a gap", 1, true, false},
        {"never", 0, false, true},
        {"a shortcut bar, the next frame", 0, false, false, 55},
        {"a shortcut bar, a frame late", 1, false, false, 55},
    };
    for (const Arrival& a : arrivals) {
        firstSwitch(PrVec2{1376, 1032}, "iPad 13-inch", a);
        firstSwitch(PrVec2{402, 874}, "iPhone 17 Pro", a);
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("keyboard frames: ok");
    return 0;
}
