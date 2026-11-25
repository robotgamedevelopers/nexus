// Unit test of the first switch from the Dashboard to an AI's tab as the iOS window draws it (FrameHost::buildAbove,
// main_ios.cpp): the window's frame code (frame_host.h) over a kit with no window, the server's AIs through a fake link,
// and the on-screen keyboard as Prax's UIKit backend gives it. The backend raises the keyboard while the last frame
// edited text and lowers it when it stopped (app_uikit.mm deliverFrame, uikit_view.mm setEditing), and the keyboard's
// rect reaches the frames later, from the system's notification. Here the rect comes the frame after the keyboard rose,
// a frame early (in the frame that switches), two frames late, with a frame's gap in it, or never (a hardware
// keyboard), as a full keyboard or a hardware keyboard's shortcut bar, and the device is turned halfway through. Each
// case is a fresh window: its first AI tab and the keyboard's first rise.
//
// The window's panel is padded by the frame's safe area, as Prax's Studio window pads it (studio_window.cpp build), and
// the prompt's whole frame stays inside the safe area in every frame: with the keyboard never up, up then lowered, up
// then dismissed by a tap outside the prompt, and turned with the keyboard down. The window was built in a box as tall
// as the whole frame while no keyboard showed, so its foot, the prompt, ran past the screen's by the safe area's top
// and bottom.
//
// A tap outside the prompt's box ends editing and lowers the keyboard, as in other iOS apps (View::tapEndsEditing); a
// drag or a scroll in the transcript, a tap in the prompt's box and a tap on a tab do not; the draft stays; and the 300
// frames after the dismiss show no change.
//
// Nothing may move once the switch is done: the focus and the keyboard each change at most once, and after they
// settle no frame asks for another. In every frame with a rect, the prompt's foot is at or above the keyboard's top. The window built straight into its root while no keyboard showed and inside a box
// above the keyboard while one did, so every node was new each time the rect came or went: the prompt took the focus
// anew, and with the rect a frame behind, the keyboard went up and down every frame.
//
// No clock (the frames' time is made up), no process, no socket, no network.

#include <algorithm>
#include <cstdio>
#include <cstring>
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

// A device: its window's extent standing upright, and its safe area upright and turned.
struct Device {
    const char* name;
    PrVec2 size;
    PrAppInsets upright;    // top, right, bottom, left
    PrAppInsets turned;
};

const Device kIPad{"iPad 13-inch", {1032, 1376}, {24, 0, 20, 0}, {24, 0, 20, 0}};
const Device kIPhone{"iPhone 17 Pro", {402, 874}, {62, 0, 34, 0}, {0, 62, 21, 62}};

struct Window {
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    Look look;
    Model model{std::vector<AIInfo>{}};
    View view;
    FrameHost host;
    test::FakeLink link;
    double time = 1000;
    PrVec2 extent;
    Device device;
    PrAppInsets safe{};
    bool hidden = false;        // the keyboard was lowered by the system while the prompt still edits (its own key)
    int frameNo = 0;
    bool editing = false;       // the last frame edited text: the backend's keyboard is up
    int rose = -1;              // the frame after the one that first edited text, while editing; -1: down
    bool wantsFrame = false;    // the last frame asked for another

    explicit Window(const Device& d, bool turned = false)
        : extent(turned ? PrVec2{d.size.y, d.size.x} : d.size), device(d), safe(turned ? d.turned : d.upright) {
        view.tapEndsEditing = true;
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
        link.dashboards({{"Dashboard", "http://dashboard.test/"}});
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
        if (a.never || hidden) return {};
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
        const float room = roomAboveKeyboard(extent, safe, rect);
        host.beginFrame(kit, fonts, time, prAppDisplayDefault());
        PrUiFrameDesc desc{.time = time, .delta = 1.0 / 120, .extent = extent, .maxRounds = 4,
            .display = prAppDisplayDefault(), .preferences = {}};
        struct Round { Window* w; PrVec2 box; } round{this, contentBox(extent, safe, room)};
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Round& r = *static_cast<Round*>(context);
            Window& w = *r.w;
            const PrLayoutNode root = prKitPanel(w.kit, kPrLayoutNodeNone, {}, PrKitPanelStyle{
                .box = {.overflow = PrKitOverflow_Clip,
                    .padding = {w.safe.top, w.safe.right, w.safe.bottom, w.safe.left},
                    .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
                .background = PrKitBackground(PrKitBackground_App)});
            w.host.buildAbove(w.kit, root, r.box);
        }, &round);
        const PrAppOutput out = prUiOutput(ui);
        ++frameNo;
        if (out.hasTextInput && !editing) rose = frameNo;
        if (!out.hasTextInput) rose = -1;
        editing = out.hasTextInput;
        wantsFrame = out.wantsFrame;
    }

    // The device turned: the extent swaps, and the safe area is the turned one's or the upright one's.
    void turn() {
        extent = PrVec2{extent.y, extent.x};
        safe = extent.x > extent.y ? device.turned : device.upright;
    }

    // The prompt's whole frame, its field and the box it shows in, is inside the safe area (true while it has none).
    bool promptInsideSafeArea() const {
        for (const PrRect p : {promptBounds(), boundsOf(view.promptBox)}) {
            if (p.bottom <= p.top) continue;
            constexpr float e = 0.5f;
            if (p.left < safe.left - e || p.top < safe.top - e || p.right > extent.x - safe.right + e ||
                    p.bottom > extent.y - safe.bottom + e)
                return false;
        }
        return true;
    }

    // An event as the Studio window hands it over: to the host, and on to the window's tree unless the host took it
    // (main.cpp).
    void send(PrAppEvent e) {
        e.time = time;
        if (!host.event(e)) prUiPushEvent(ui, kPrUiViewportMain, &e);
    }

    // A pointer event, and the frame that takes it unless not `draw`.
    void pointer(PrAppEventKind kind, PrVec2 at, bool draw = true) {
        PrAppEvent e{};
        e.kind = kind;
        e.time = time;
        e.pointer.position = at;
        e.pointer.primary = true;
        e.pointer.clicks = kind == PrAppEventKind_PointerDown ? 1 : 0;
        send(e);
        if (draw) frame(Arrival{"the next frame", 0, false, false});
    }

    // A press and its release where it went down; the frame after the release is the caller's unless `draw`.
    void tap(PrVec2 at, bool draw = true) {
        pointer(PrAppEventKind_PointerDown, at);
        pointer(PrAppEventKind_PointerUp, at, draw);
    }

    void type(const char* text) {
        PrAppEvent e{};
        e.kind = PrAppEventKind_Text;
        e.time = time;
        e.input.text = PrStringView{text, std::strlen(text)};
        send(e);
    }

    // A point in the transcript: across the middle of the content, a third of the way down, above the prompt.
    PrVec2 transcriptPoint() const {
        const PrRect p = promptBounds();
        const float top = safe.top + 60;
        return PrVec2{extent.x / 2, top + (p.top - top) / 2};
    }

    PrVec2 promptPoint() const {
        const PrRect p = promptBounds();
        return PrVec2{(p.left + p.right) / 2, (p.top + p.bottom) / 2};
    }

    PrRect boundsOf(PrLayoutNode node) const {
        if (node == kPrLayoutNodeNone) return {};
        return prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
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
void firstSwitch(const Device& d, const Arrival& a) {
    Window w(d, true);
    const char* device = d.name;
    for (int i = 0; i < 10; ++i) w.frame(a);
    CHECK(w.model.tab_count() > 0 && w.model.tab(w.model.active()).kind == TabKind::Dashboard);
    CHECK(!w.editing);                      // the Dashboard edits no text: the keyboard is down
    const std::size_t rowan = w.tabOf("rowan");
    CHECK(rowan < w.model.tab_count());
    if (rowan >= w.model.tab_count()) return;
    w.model.select_tab(rowan);
    PrLayoutNode focus = prUiFocus(w.ui);
    bool editing = w.editing;
    int refocused = 0, toggled = 0, askedLate = 0, covered = 0, outside = 0;
    for (int i = 0; i < 300; ++i) {
        if (i == 150) {
            // The device turned: the window's extent swaps, and its next frames settle the same way.
            w.turn();
        }
        w.frame(a, i == 0);
        outside += !w.promptInsideSafeArea();
        const PrRect prompt = w.promptBounds();
        const bool shows = w.rect.bottom > w.rect.top;
        if (shows && prompt.bottom > prompt.top && prompt.bottom > w.rect.top) ++covered;
        refocused += prUiFocus(w.ui) != focus;
        toggled += w.editing != editing;
        focus = prUiFocus(w.ui);
        editing = w.editing;
        if ((i >= 30 && i < 150) || i >= 180) askedLate += w.wantsFrame;
    }
    if (refocused > 1 || toggled > 1 || askedLate != 0 || covered != 0 || outside != 0) {
        std::fprintf(stderr, "%s, rect %s: of 300 frames, the focus changed in %d, the keyboard in %d, %d of the "
            "settled ones asked for another, the keyboard covered the prompt in %d, and it left the safe area in %d\n",
            device, a.name, refocused, toggled, askedLate, covered, outside);
    }
    CHECK(refocused <= 1);
    CHECK(toggled <= 1);
    CHECK(askedLate == 0);
    CHECK(covered == 0);
    CHECK(outside == 0);
    CHECK(w.editing);                       // the prompt has the focus: the keyboard is up
    CHECK(focus != kPrLayoutNodeNone && focus == w.view.prompt);
    const float inside = w.extent.y - w.safe.top - w.safe.bottom;
    const float above = w.extent.y - keyboardHeight(a, w.extent) - w.safe.top;
    const float shown = a.never ? inside : std::min(above, inside);
    CHECK(w.view.extent.y == shown);        // the window stands above the keyboard once its rect came
    CHECK(w.view.extent.x == w.extent.x - w.safe.left - w.safe.right);
}

const Arrival kNextFrame{"the next frame", 0, false, false};

// A window on an AI's tab with the keyboard up and settled, the prompt holding a draft typed into it.
void openOnPrompt(Window& w, const char* draft = nullptr) {
    for (int i = 0; i < 10; ++i) w.frame(kNextFrame);
    w.model.select_tab(w.tabOf("rowan"));
    for (int i = 0; i < 30; ++i) w.frame(kNextFrame, i == 0);
    if (draft != nullptr) {
        w.type(draft);
        for (int i = 0; i < 5; ++i) w.frame(kNextFrame);
    }
}

// What 300 frames do after a moment: how often the focus and the keyboard changed, how many frames from the settled one on
// asked for another, and in how many the prompt left the safe area. The device turns at turnAt (-1: never), and the
// frames just after a turn may ask for more.
struct Still { int refocused = 0, toggled = 0, asked = 0, outside = 0; };

Still drawStill(Window& w, int frames = 300, int settled = 30, int turnAt = -1) {
    Still s;
    PrLayoutNode focus = prUiFocus(w.ui);
    bool editing = w.editing;
    for (int i = 0; i < frames; ++i) {
        if (i == turnAt) w.turn();
        w.frame(kNextFrame);
        s.refocused += prUiFocus(w.ui) != focus;
        s.toggled += w.editing != editing;
        focus = prUiFocus(w.ui);
        editing = w.editing;
        const bool turning = turnAt >= 0 && i >= turnAt && i < turnAt + 30;
        if (i >= settled && !turning) s.asked += w.wantsFrame;
        s.outside += !w.promptInsideSafeArea();
    }
    return s;
}

// The keyboard never up (a hardware keyboard), up then lowered by its own key, and turned while down: the prompt's
// frame stays in the safe area, its foot near the safe area's.
void keyboardDown(const Device& d) {
    {
        Window w(d);
        for (int i = 0; i < 10; ++i) w.frame(Arrival{"never", 0, false, true});
        w.model.select_tab(w.tabOf("rowan"));
        int outside = 0;
        for (int i = 0; i < 300; ++i) {
            if (i == 150) w.turn();
            w.frame(Arrival{"never", 0, false, true});
            outside += !w.promptInsideSafeArea();
        }
        if (outside != 0) std::fprintf(stderr, "%s, keyboard never up: the prompt left the safe area in %d of 300 frames\n", d.name, outside);
        CHECK(outside == 0);
        const PrRect p = w.boundsOf(w.view.promptBox);    // the box the prompt shows in
        CHECK(p.bottom > p.top);
        if (p.bottom < w.extent.y - w.safe.bottom - 40)
            std::fprintf(stderr, "%s, keyboard never up: the prompt's foot is at %g of %g, the safe area's at %g\n",
                d.name, p.bottom, w.extent.y, w.extent.y - w.safe.bottom);
        CHECK(p.bottom >= w.extent.y - w.safe.bottom - 40);    // at the foot, not halfway up
    }
    {
        Window w(d);
        openOnPrompt(w);
        CHECK(w.rect.bottom > w.rect.top);
        w.hidden = true;                    // the keyboard's own key lowers it: the prompt still edits
        const Still s = drawStill(w, 300, 30, 150);
        if (s.outside != 0) std::fprintf(stderr, "%s, keyboard up then down: the prompt left the safe area in %d of 300 frames\n", d.name, s.outside);
        CHECK(s.outside == 0);
        CHECK(w.view.extent.y == w.extent.y - w.safe.top - w.safe.bottom);
    }
}

// A tap outside the prompt ends editing; the draft stays; the 300 frames after it show no change, the device turning
// halfway through them with the keyboard down; and a tap back in the prompt's box takes editing up again.
void tapOutside(const Device& d) {
    Window w(d);
    openOnPrompt(w, "half a thought");
    const std::size_t rowan = w.tabOf("rowan");
    CHECK(w.model.tab(rowan).draft == "half a thought");
    CHECK(w.editing);
    const PrVec2 at = w.transcriptPoint();
    const PrRect box = w.boundsOf(w.view.promptBox);
    CHECK(!(at.x >= box.left && at.x < box.right && at.y >= box.top && at.y < box.bottom));
    w.tap(at, false);
    const Still s = drawStill(w, 300, 2, 150);
    if (s.refocused > 1 || s.toggled != 1 || s.asked != 0 || s.outside != 0) {
        std::fprintf(stderr, "%s, tap outside: of 300 frames, the focus changed in %d, the keyboard in %d, %d of the "
            "settled ones asked for another, and the prompt left the safe area in %d\n", d.name, s.refocused,
            s.toggled, s.asked, s.outside);
    }
    CHECK(s.toggled == 1);                  // the keyboard went down once, and stayed down
    CHECK(s.refocused <= 1);
    CHECK(s.asked == 0);
    CHECK(s.outside == 0);
    CHECK(!w.editing);
    CHECK(prUiFocus(w.ui) != w.view.prompt);
    CHECK(w.rect.bottom <= w.rect.top);     // no keyboard over the window
    CHECK(w.model.tab(rowan).draft == "half a thought");
    // A tap in the prompt's box takes editing up again, with the draft.
    w.tap(w.promptPoint());
    for (int i = 0; i < 5; ++i) w.frame(kNextFrame);
    CHECK(w.editing);
    CHECK(prUiFocus(w.ui) == w.view.prompt);
    CHECK(w.model.tab(rowan).draft == "half a thought");
}

// A drag through the transcript, a scroll in it, a tap in the prompt's box and a tap on another tab end no editing.
void notATapOutside(const Device& d) {
    {
        Window w(d);
        openOnPrompt(w, "kept");
        const PrVec2 at = w.transcriptPoint();
        w.pointer(PrAppEventKind_PointerDown, at);
        w.pointer(PrAppEventKind_PointerMove, PrVec2{at.x, at.y - 20});
        w.pointer(PrAppEventKind_PointerMove, PrVec2{at.x, at.y - 60});
        w.pointer(PrAppEventKind_PointerUp, PrVec2{at.x, at.y - 60});
        const Still s = drawStill(w, 60);
        CHECK(w.editing);                   // a drag is no tap
        CHECK(s.toggled == 0);
        CHECK(prUiFocus(w.ui) == w.view.prompt);
    }
    {
        Window w(d);
        openOnPrompt(w, "kept");
        const PrVec2 at = w.transcriptPoint();
        w.pointer(PrAppEventKind_PointerDown, at);
        PrAppEvent e{};
        e.kind = PrAppEventKind_Scroll;
        e.scroll.position = at;
        e.scroll.delta = PrVec2{0, -40};
        e.scroll.unit = PrAppScrollUnit_Pixel;
        w.send(e);
        w.pointer(PrAppEventKind_PointerUp, at);
        const Still s = drawStill(w, 60);
        CHECK(w.editing);                   // a scroll is no tap
        CHECK(s.toggled == 0);
    }
    {
        Window w(d);
        openOnPrompt(w, "kept");
        w.tap(w.promptPoint());
        const Still s = drawStill(w, 60);
        CHECK(w.editing);                   // a tap in the prompt's box, where the dictation key is, edits on
        CHECK(s.toggled == 0);
        CHECK(prUiFocus(w.ui) == w.view.prompt);
        CHECK(w.model.tab(w.tabOf("rowan")).draft == "kept");
    }
    {
        Window w(d);
        openOnPrompt(w, "kept");
        const std::size_t stella = w.tabOf("stella");
        CHECK(stella < w.view.tabNodes.size());
        if (stella >= w.view.tabNodes.size()) return;
        const PrRect tab = w.boundsOf(w.view.tabNodes[stella]);
        CHECK(tab.bottom > tab.top);
        w.tap(PrVec2{(tab.left + tab.right) / 2, (tab.top + tab.bottom) / 2});
        drawStill(w, 60);
        CHECK(w.model.active() == stella);  // the tab switched, as it does
        CHECK(w.editing);                   // and its prompt edits
        CHECK(prUiFocus(w.ui) == w.view.prompt);
        CHECK(w.model.tab(w.tabOf("rowan")).draft == "kept");
    }
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
        firstSwitch(kIPad, a);
        firstSwitch(kIPhone, a);
    }
    for (const Device& d : {kIPad, kIPhone}) {
        keyboardDown(d);
        tapOutside(d);
        notATapOutside(d);
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("keyboard frames: ok");
    return 0;
}
