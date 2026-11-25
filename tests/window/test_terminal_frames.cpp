// Unit test of a terminal tab whose shell runs on the server (src/remote_terminals.h), shown frame after frame with no
// input, as the iOS window draws it: the window's frame code (frame_host.h) over a kit with no window, the server's
// state through a fake link, and the on-screen keyboard as iOS gives it, shown while the last frame edited text and
// laid out above (FrameHost::buildAbove, main_ios.cpp). Nothing may move while nothing happens: the focus stays where
// it is, the keyboard stays up, the tab's title keeps its place, and no frame asks for another. The window built
// straight into its root while no keyboard showed, and above the keyboard while one did, so its nodes were new ones
// each time the keyboard came or went: the terminal lost the focus, took it back, and the keyboard went up and down
// every frame (FrameHost::buildAbove).
//
// No clock (the frames' time is made up), no process, no socket, no network: the screen is painted from the state.

#include <cstdio>
#include <memory>
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
#include "model.h"
#include "remote_terminals.h"
#include "sync/sessions.h"
#include "sync/terminal_state.h"
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

constexpr std::uint64_t kEpoch = 7;
constexpr float kKeyboard = 320;    // the on-screen keyboard's height over the window's foot, points

sync::TerminalRef term(std::uint64_t n, std::string opener, std::string title) {
    auto v = std::make_shared<sync::TerminalView>();
    v->id = (kEpoch << 32) | n;
    v->opener = std::move(opener);
    v->title = std::move(title);
    v->columns = 80;
    v->rows = 24;
    v->lines.assign(24, std::make_shared<const std::string>());
    v->lines[0] = std::make_shared<const std::string>("demo$ ");
    return v;
}

struct Window {
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    Look look;
    Model model{std::vector<FriendInfo>{}, true};
    View view;
    std::unique_ptr<RemoteTerminals> terminals;
    FrameHost host;
    test::FakeLink link;
    double time = 1000;
    PrVec2 extent;
    bool keyboard = false;      // the on-screen keyboard shows: the last frame edited text
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
        terminals = std::make_unique<RemoteTerminals>(nullptr, fonts, "tok");
        terminals->setLink(&link);
        terminals->setKit(kit);
        view.terminals = terminals.get();
        view.newTerminalButton = true;
        host.model = &model;
        host.view = &view;
        host.ui = ui;
        host.connect(&link, "server.test");
    }

    ~Window() {
        view.terminals = nullptr;
        terminals.reset();
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prFontSystemDestroy(system);
    }

    // The server's state: one terminal, opened by another window, which this window takes into a tab of its own.
    void serve() {
        LinkState s;
        s.phase = sync::LinkPhase::Connected;
        s.sessions.setTerminals(true, kEpoch, {term(1, "ipad/1", "demo")});
        s.sessions.setNextAction(1);
        link.state = std::move(s);
        link.changed = true;
    }

    // One frame as the iOS window makes it (main_ios.cpp): the host's frame start, then the rounds, each built above
    // the keyboard while it shows.
    void frame() {
        time += 1.0 / 120;
        host.beginFrame(kit, fonts, time, prAppDisplayDefault());
        const float cover = keyboard ? kKeyboard : 0;
        PrUiFrameDesc desc{.time = time, .delta = 1.0 / 120, .extent = extent, .maxRounds = 4,
            .display = prAppDisplayDefault(), .preferences = {}};
        struct Round { Window* w; float cover; } round{this, cover};
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Round& r = *static_cast<Round*>(context);
            Window& w = *r.w;
            const PrLayoutNode root = prKitPanel(w.kit, kPrLayoutNodeNone, {}, PrKitPanelStyle{
                .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100),
                    .height = prKitPercent(100)}},
                .background = PrKitBackground(PrKitBackground_App)});
            w.host.buildAbove(w.kit, root, w.extent, r.cover);
        }, &round);
        const PrAppOutput out = prUiOutput(ui);
        keyboard = out.hasTextInput;
        wantsFrame = out.wantsFrame;
    }

    PrRect titleBounds() const {
        const std::size_t active = model.active();
        if (active >= view.tabNodes.size() || view.tabNodes[active] == kPrLayoutNodeNone) return {};
        return prLayoutRectToPx(prLayoutNodeBounds(tree, view.tabNodes[active], PrLayoutCoordinates_Viewport));
    }
};

bool same(PrRect a, PrRect b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// A terminal tab shown with no input: after the frames that open it, the focus, the keyboard and the title stay put,
// and the window stops asking for frames.
void stillWithNoInput(PrVec2 size, const char* what) {
    Window w(size);
    w.serve();
    w.frame();
    for (std::size_t i = 0; i < w.model.tab_count(); ++i) {
        if (w.model.tab(i).kind == TabKind::Terminal) w.model.select_tab(i);
    }
    for (int i = 0; i < 30; ++i) w.frame();
    CHECK(w.model.tab_count() > 0 && w.model.tab(w.model.active()).kind == TabKind::Terminal);
    const PrLayoutNode focus = prUiFocus(w.ui);
    const bool keyboard = w.keyboard;
    const PrRect title = w.titleBounds();
    CHECK(focus != kPrLayoutNodeNone);
    CHECK(keyboard);                        // the terminal edits text: the keyboard is up
    CHECK(title.right > title.left);
    int moved = 0, refocused = 0, toggled = 0, asked = 0;
    for (int i = 0; i < 300; ++i) {
        w.frame();
        moved += !same(w.titleBounds(), title);
        refocused += prUiFocus(w.ui) != focus;
        toggled += w.keyboard != keyboard;
        asked += w.wantsFrame;
    }
    if (moved || refocused || toggled || asked) {
        std::fprintf(stderr, "%s: of 300 frames, title moved in %d, focus changed in %d, keyboard changed in %d, "
            "another frame asked in %d\n", what, moved, refocused, toggled, asked);
    }
    CHECK(moved == 0);
    CHECK(refocused == 0);
    CHECK(toggled == 0);
    CHECK(asked == 0);
}

} // namespace

int main() {
    stillWithNoInput(PrVec2{1032, 1376}, "iPad 13-inch");
    stillWithNoInput(PrVec2{402, 874}, "iPhone 17 Pro");
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("terminal frames: ok");
    return 0;
}
