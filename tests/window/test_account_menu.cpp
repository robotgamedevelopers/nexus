// Unit tests of switching an AI's account (src/account_menu.h, docs/accounts.md "Switching accounts"): the menu that a
// press on the active AI's tab opens under the tab's highlight, its one gesture (press, slide, lift), its layout, and the
// /account, /login and /logout commands. The window's real frame code (frame_host.h) over a kit with no window, the
// server's AIs and accounts through a fake link, the pointer and keys as the window gets them. Each test names the
// plant that turns it red.
//
// No clock (the frames' time is made up), no process, no socket, no network.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_panel.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/ui.h"

#include "account_menu.h"
#include "frame_host.h"
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

constexpr sync::SessionId kRowan = 7, kStella = 8;
const std::string kLong = "a.person.with.a.really.long.work.address.for.the.menu@robotgamedevelopers-example.test";

PrAppEvent pointerEvent(PrAppEventKind kind, PrVec2 at) {
    PrAppEvent e{};
    e.kind = kind;
    e.pointer.position = at;
    e.pointer.primary = true;
    e.pointer.type = PrAppPointerType_Touch;
    return e;
}

PrAppEvent keyEvent(PrAppEventKind kind, PrAppKey key, PrAppKeyCode code) {
    PrAppEvent e{};
    e.kind = kind;
    e.keyboard.key = key;
    e.keyboard.code = code;
    return e;
}

// What a label looks like, as the layout has it: its text, font, size, weight, colour and frame.
struct Looks {
    std::string text;
    const void* font = nullptr;
    float size = 0, weight = 0;
    std::uint64_t color = 0;
    PrRect frame{};
    bool operator==(const Looks& o) const {
        return text == o.text && font == o.font && size == o.size && weight == o.weight && color == o.color &&
               frame.left == o.frame.left && frame.top == o.frame.top && frame.right == o.frame.right &&
               frame.bottom == o.frame.bottom;
    }
};

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
    bool editing = false;
    bool wantsFrame = false;
    std::vector<sync::AccountChoice> rowan{
        {"a1", "Home", false, "person@home.test"},
        {"a2", "Work", false, kLong},
        {"a3", "Spare", true, "spare@example.test"},
    };
    std::string current = "a1";
    std::vector<sync::SignInOffer> offers;
    std::uint64_t next = 1;

    explicit Window(PrVec2 size = PrVec2{1376, 1032}) : extent(size) {
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
        publish();
        for (int i = 0; i < 3; ++i) frame();
        model.select_tab(tabOf("rowan"));
        for (int i = 0; i < 3; ++i) frame();
    }

    ~Window() {
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prFontSystemDestroy(system);
    }

    // The server's state: rowan's session on `current`, stella's with no accounts; the actions applied so far.
    void publish() {
        auto r = std::make_shared<sync::SessionView>();
        r->id = kRowan;
        r->info = {"rowan", "claude", current, "", ""};
        auto s = std::make_shared<sync::SessionView>();
        s->id = kStella;
        s->info = {"stella", "claude", "", "", ""};
        link.state.phase = sync::LinkPhase::Connected;
        link.state.sessions = sync::Sessions({r, s});
        link.state.sessions.setAIs({{"rowan", rowan}, {"stella", {}}});
        link.state.sessions.setOffers(offers);
        link.state.sessions.setNextAction(link.sent.size() + 1);
        link.changed = true;
    }

    void frame() {
        time += 1.0 / 120;
        host.beginFrame(kit, fonts, time, prAppDisplayDefault());
        PrUiFrameDesc desc{.time = time, .delta = 1.0 / 120, .extent = extent, .maxRounds = 4,
            .display = prAppDisplayDefault(), .preferences = {}};
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Window& w = *static_cast<Window*>(context);
            const PrLayoutNode root = prKitPanel(w.kit, kPrLayoutNodeNone, {}, PrKitPanelStyle{
                .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100),
                    .height = prKitPercent(100)}},
                .background = PrKitBackground(PrKitBackground_App)});
            w.host.buildAbove(w.kit, root, w.extent);
        }, this);
        const PrAppOutput out = prUiOutput(ui);
        editing = out.hasTextInput;
        wantsFrame = out.wantsFrame;
    }

    // An event as the window gets it: the host first, then the tree (main_ios.cpp event, Prax's studio window).
    void send(PrAppEvent e) {
        e.time = time;
        if (!host.event(e)) prUiPushEvent(ui, kPrUiViewportMain, &e);
    }
    void press(PrVec2 at) { send(pointerEvent(PrAppEventKind_PointerDown, at)); frame(); }
    void slide(PrVec2 at) { send(pointerEvent(PrAppEventKind_PointerMove, at)); frame(); }
    void lift(PrVec2 at) { send(pointerEvent(PrAppEventKind_PointerUp, at)); frame(); }
    void enter() {
        send(keyEvent(PrAppEventKind_KeyDown, PrAppKey_Enter, PrAppKeyCode_Enter));
        send(keyEvent(PrAppEventKind_KeyUp, PrAppKey_Enter, PrAppKeyCode_Enter));
        frame();
    }
    void command(const std::string& text) {
        model.set_draft(model.active(), text);
        view.prompted = ~std::size_t(0);
        frame();
        enter();
    }

    std::size_t tabOf(const std::string& name) const {
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            if (model.tab(i).kind == TabKind::AI && model.tab(i).name == name) return i;
        }
        return model.tab_count();
    }
    PrRect bounds(PrLayoutNode node) const {
        if (node == kPrLayoutNodeNone || !prLayoutNodeIsValid(tree, node)) return {};
        return prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    }
    PrVec2 centre(PrLayoutNode node) const {
        const PrRect b = bounds(node);
        return PrVec2{(b.left + b.right) / 2, (b.top + b.bottom) / 2};
    }
    PrLayoutNode tabNode(const std::string& name) const {
        const auto i = tabOf(name);
        return i < view.tabNodes.size() ? view.tabNodes[i] : kPrLayoutNodeNone;
    }
    // The first text node under `node`.
    PrLayoutNode textIn(PrLayoutNode node) const {
        if (node == kPrLayoutNodeNone) return node;
        if (prLayoutNodeKind(tree, node) == PrLayoutNodeKind_Text) return node;
        for (PrLayoutNode n = prLayoutNodeFirstChild(tree, node); n != kPrLayoutNodeNone; n = prLayoutNodeNextSibling(tree, n)) {
            const auto found = textIn(n);
            if (found != kPrLayoutNodeNone) return found;
        }
        return kPrLayoutNodeNone;
    }
    Looks looks(PrLayoutNode node) const {
        Looks l;
        const PrLayoutNode text = textIn(node);
        if (text == kPrLayoutNodeNone) return l;
        const auto t = prLayoutNodeText(tree, text);
        l.text.assign(t.data, t.size);
        // A text run has no style of its own: its element (the parent) holds its font and colour.
        const auto record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, prLayoutNodeParent(tree, text)));
        const auto font = prLayoutFontStyleRecord(tree, record.groups[PrLayoutGroup_Font]);
        l.font = font.font;
        if (font.font) {
            const PrFontStyle s = prTextFontStyle(font.font);
            l.size = s.size;
            l.weight = s.weight;
        }
        const auto color = prLayoutColorStyleRecord(tree, record.groups[PrLayoutGroup_Color]);
        std::memcpy(&l.color, &color.color, std::min(sizeof l.color, sizeof color.color));
        l.frame = bounds(text);
        return l;
    }
    // The switches sent, as "session:account": the server's login switch, a SignInClaim with this window's token and no
    // request (src/sync/hub.cpp). A SetAccount shows as "set:session:account", so a test expecting claims fails on one.
    std::vector<std::string> switches() const {
        std::vector<std::string> out;
        for (const auto& a : link.sent) {
            if (a.kind == sync::ActionKind::SetAccount) out.push_back("set:" + std::to_string(a.session) + ":" + a.text);
            if (a.kind == sync::ActionKind::SignInClaim)
                out.push_back(std::to_string(a.session) + ":" + a.account + (a.request ? ":request" : "") +
                              (a.text.empty() ? ":no-token" : ""));
        }
        return out;
    }
    bool menuShows() const { return view.accounts.node != kPrLayoutNodeNone; }
    // The point on row i straight below the press, from: a finger slides down from the tab onto a row.
    PrVec2 below(PrVec2 from, std::size_t i) const {
        return PrVec2{from.x, centre(view.accounts.rowNodes[i]).y};
    }
};

// A press on the active AI's tab opens the menu in that same frame. Plant: open on the lift instead.
void pressOnTheActiveTabOpensTheMenu() {
    Window w;
    CHECK(!w.menuShows());
    w.press(w.centre(w.tabNode("rowan")));
    CHECK(w.menuShows());
    CHECK(w.view.accounts.rowNodes.size() == 3);    // a1, a2, Sign in to another account…
    w.lift(w.centre(w.tabNode("rowan")));
}

// Press, slide down to the second row and lift there: rowan's session switches to the second account, a2. Plant: choose
// the row under the press rather than under the lift.
void pressSlideAndLiftSwitches() {
    Window w;
    const PrVec2 at = w.centre(w.tabNode("rowan"));
    w.press(at);
    CHECK(w.view.accounts.rowNodes.size() == 3);
    if (w.view.accounts.rowNodes.size() != 3) return;
    const PrVec2 row2 = w.below(at, 1);
    w.slide(PrVec2{row2.x, row2.y - 10});
    w.slide(row2);
    CHECK(w.view.accounts.gesture.hovered() == std::optional<std::size_t>(1));
    w.lift(row2);
    CHECK(!w.menuShows());
    CHECK((w.switches() == std::vector<std::string>{"7:a2"}));
    CHECK(w.model.tab(w.model.active()).name == "rowan");
}

// Press and lift without moving: the menu closes and nothing changes. Plant: leave the menu open after a lift off a row.
void pressAndLiftClosesAndChangesNothing() {
    Window w;
    const PrVec2 at = w.centre(w.tabNode("rowan"));
    w.press(at);
    CHECK(w.menuShows());
    w.lift(at);
    CHECK(!w.menuShows());
    CHECK(!w.view.accounts.gesture.open());
    CHECK(w.switches().empty());
    CHECK(w.model.tab(w.model.active()).name == "rowan");
    // Lifting below the menu's last row, off every row, changes nothing either.
    w.press(at);
    const auto menu = w.bounds(w.view.accounts.node);
    w.slide(PrVec2{at.x, menu.bottom + 40});
    w.lift(PrVec2{at.x, menu.bottom + 40});
    CHECK(!w.menuShows() && w.switches().empty());
}

// Press, slide down onto the menu, then across the wide row to its far end: the press went down first, so it stays the
// menu's, the row stays lit, and the lift chooses it; the tab never moves. Plant: let a sideways move close the menu
// after the press went down (no deciding, AccountMenuGesture::move).
void downThenAcrossStaysTheMenu() {
    Window w;
    const PrVec2 at = w.centre(w.tabNode("rowan"));
    const auto order = w.tabOf("rowan");
    w.press(at);
    if (w.view.accounts.rowNodes.size() != 3) {
        CHECK(w.view.accounts.rowNodes.size() == 3);
        return;
    }
    const PrVec2 down = w.below(at, 1);
    w.slide(down);
    const PrRect row = w.bounds(w.view.accounts.rowNodes[1]);
    const PrVec2 across{row.right - 4, down.y};
    w.slide(across);
    CHECK(w.menuShows());
    CHECK(w.view.accounts.gesture.hovered() == std::optional<std::size_t>(1));
    w.lift(across);
    CHECK((w.switches() == std::vector<std::string>{"7:a2"}));
    CHECK(w.tabOf("rowan") == order);
}

// Press, then a drag mostly sideways past the threshold: the menu closes and the tab moves, as a tab's drag always did.
// Plant: keep the menu (and the press) when the movement is sideways.
void pressThenSidewaysDragMovesTheTab() {
    Window w;
    const auto before = w.model.tab(w.tabOf("rowan")).id;
    CHECK(w.tabOf("rowan") + 1 == w.tabOf("stella"));
    const PrVec2 start = w.centre(w.tabNode("rowan"));
    const PrRect stella = w.bounds(w.tabNode("stella"));
    w.press(start);
    CHECK(w.menuShows());
    w.slide(PrVec2{start.x + 20, start.y + 2});
    CHECK(!w.menuShows());
    w.slide(PrVec2{stella.right - 2, start.y});
    w.lift(PrVec2{stella.right - 2, start.y});
    CHECK(w.tabOf("stella") + 1 == w.tabOf("rowan"));
    CHECK(w.model.tab(w.model.active()).id == before);
    CHECK(w.switches().empty());
}

// A press on another AI's tab never opens the menu: it selects that tab, as ever. Plant: open on any friend tab.
void pressOnAnotherTabNeverOpens() {
    Window w;
    const PrVec2 at = w.centre(w.tabNode("stella"));
    w.press(at);
    CHECK(!w.menuShows());
    w.lift(at);
    CHECK(!w.menuShows());
    CHECK(w.model.tab(w.model.active()).name == "stella");
}

// The current account is marked, and only it. Plant: mark every row.
void theCurrentAccountIsMarked() {
    Window w;
    w.press(w.centre(w.tabNode("rowan")));
    const auto& rows = w.view.accounts.rows;
    CHECK(rows.size() == 3);
    if (rows.size() != 3) return;
    CHECK(rows[0].account == "a1" && rows[0].current);
    CHECK(rows[1].account == "a2" && !rows[1].current);
    CHECK(rows[2].signIn && !rows[2].current && rows[2].text == std::string(kSignInAnotherRow));
    // The mark is drawn in the current row alone: its row holds its text and the mark.
    const auto children = [&](PrLayoutNode n) {
        int count = 0;
        for (PrLayoutNode c = prLayoutNodeFirstChild(w.tree, n); c != kPrLayoutNodeNone; c = prLayoutNodeNextSibling(w.tree, c))
            ++count;
        return count;
    };
    CHECK(children(w.view.accounts.rowNodes[0]) == 2);
    CHECK(children(w.view.accounts.rowNodes[1]) == 1);
    CHECK(children(w.view.accounts.rowNodes[2]) == 1);
    w.lift(w.centre(w.tabNode("rowan")));
}

// The menu's layout: its top just below the tab's highlight and its left edge on the highlight's; no part of it over the
// highlight or the label; each row at least 44 points; each row's text no bigger than the tab label's and starting
// where the label's text starts; a long email cut with "…" on one line. Plants: the menu over the tab (top at the
// highlight's top), rows of 30 points, the rows' text a role bigger than the tab label's, no cut.
void theMenuSitsUnderTheHighlight(PrVec2 size, const char* device) {
    Window w(size);
    const PrLayoutNode tab = w.tabNode("rowan");
    const PrRect highlight = w.bounds(tab);
    const Looks label = w.looks(tab);
    w.press(w.centre(tab));
    const PrRect menu = w.bounds(w.view.accounts.node);
    const bool ok = menu.top >= highlight.bottom && std::fabs(menu.left - highlight.left) <= 0.5f &&
                    menu.top - highlight.bottom <= 8;
    if (!ok) std::fprintf(stderr, "%s: highlight %g..%g x %g..%g, menu %g..%g x %g..%g\n", device, highlight.left,
                          highlight.right, highlight.top, highlight.bottom, menu.left, menu.right, menu.top, menu.bottom);
    CHECK(ok);
    const MenuBox m{menu.left, menu.top, menu.right, menu.bottom};
    CHECK(!m.overlaps(MenuBox{highlight.left, highlight.top, highlight.right, highlight.bottom}));
    CHECK(!m.overlaps(MenuBox{label.frame.left, label.frame.top, label.frame.right, label.frame.bottom}));
    CHECK(menu.right <= size.x);
    for (std::size_t i = 0; i < w.view.accounts.rowNodes.size(); ++i) {
        const PrRect row = w.bounds(w.view.accounts.rowNodes[i]);
        CHECK(row.bottom - row.top >= 44 - 0.01f);
        const Looks text = w.looks(w.view.accounts.rowTexts[i]);
        CHECK(text.size > 0 && text.size <= label.size);
        CHECK(text.weight <= label.weight);
        CHECK(std::fabs(text.frame.left - label.frame.left) <= 1);
        CHECK(text.frame.bottom - text.frame.top < 2 * (label.frame.bottom - label.frame.top));     // one line
        CHECK(text.frame.right <= menu.right);
    }
    const Looks cut = w.looks(w.view.accounts.rowTexts[1]);
    CHECK(cut.text != kLong && cut.text.size() > 3 && cut.text.ends_with("\xE2\x80\xA6"));
    CHECK(kLong.starts_with(cut.text.substr(0, cut.text.size() - 3)));
    w.lift(w.centre(tab));
}

// The tab's label looks the same at every moment: before the press, pressed, with the menu open, mid-slide, after the
// lift that chooses, and after a tab move (its size and look; the move puts it elsewhere). Plant: the label bold, or
// another colour, while the menu is open.
void theLabelNeverChanges() {
    Window w;
    const PrLayoutNode tab = w.tabNode("rowan");
    const Looks before = w.looks(tab);
    CHECK(before.text == "Rowan" && before.size > 0);
    const PrVec2 at = w.centre(tab);
    w.press(at);
    CHECK(w.looks(tab) == before);
    w.frame();
    CHECK(w.looks(tab) == before);
    const PrVec2 row = w.below(at, 0);
    w.slide(PrVec2{at.x, (at.y + row.y) / 2});
    CHECK(w.looks(tab) == before);
    w.slide(row);
    CHECK(w.looks(tab) == before);
    w.lift(row);
    CHECK(w.looks(tab) == before);
    for (int i = 0; i < 10; ++i) w.frame();
    CHECK(w.looks(tab) == before);
    // A tab move: the label goes with its tab and looks the same.
    const PrRect stella = w.bounds(w.tabNode("stella"));
    w.press(at);
    w.slide(PrVec2{at.x + 20, at.y});
    w.slide(PrVec2{stella.right - 2, at.y});
    w.lift(PrVec2{stella.right - 2, at.y});
    for (int i = 0; i < 4; ++i) w.frame();
    const Looks moved = w.looks(w.tabNode("rowan"));
    CHECK(moved.text == before.text && moved.font == before.font && moved.size == before.size &&
          moved.weight == before.weight && moved.color == before.color);
    CHECK(std::fabs((moved.frame.right - moved.frame.left) - (before.frame.right - before.frame.left)) <= 0.5f);
}

// Still frames: with the finger down and the menu open, 300 frames with no input change nothing: not the focus, not the
// keyboard, not the menu; and once the strip has settled none asks for another (the strip's own motion after a press runs
// about 8 frames, the same as a press on any tab: the first 30 frames are not counted). The same after it closes. Plant:
// focus a row each frame (prUiSetFocus in accountDropMenu).
void stillFramesWhileTheMenuIsOpen() {
    Window w;
    w.frame();
    const PrLayoutNode focusBefore = prUiFocus(w.ui);
    const bool editingBefore = w.editing;
    const PrVec2 at = w.centre(w.tabNode("rowan"));
    w.press(at);
    CHECK(w.menuShows());
    CHECK(prUiFocus(w.ui) == focusBefore && w.editing == editingBefore);
    const PrLayoutNode menu = w.view.accounts.node;
    int refocused = 0, toggled = 0, asked = 0, moved = 0;
    for (int i = 0; i < 300; ++i) {
        w.frame();
        refocused += prUiFocus(w.ui) != focusBefore;
        toggled += w.editing != editingBefore;
        asked += i >= 30 && w.wantsFrame;
        moved += w.view.accounts.node != menu || !w.menuShows();
    }
    if (refocused || toggled || asked || moved)
        std::fprintf(stderr, "menu open, 300 still frames: focus %d, keyboard %d, asked %d, menu %d\n", refocused, toggled,
                     asked, moved);
    CHECK(refocused == 0 && toggled == 0 && asked == 0 && moved == 0);
    w.lift(at);
    CHECK(!w.menuShows());
    refocused = toggled = asked = 0;
    for (int i = 0; i < 300; ++i) {
        w.frame();
        refocused += prUiFocus(w.ui) != focusBefore;
        toggled += w.editing != editingBefore;
        asked += i >= 2 && w.wantsFrame;
    }
    CHECK(refocused == 0 && toggled == 0 && asked == 0);
}

// /account opens the dialog of rowan's signed-in accounts, the current one checked, then "Sign in to another account…";
// choosing a row switches. Plant: bind /account to nothing (window.star).
void accountCommandOpensTheSelector() {
    Window w;
    w.command("/account");
    CHECK(w.view.accounts.dialog == AccountMenuView::Dialog::Account);
    const auto& rows = w.view.accounts.dialogRows;
    CHECK(rows.size() == 3 && rows[0].account == "a1" && rows[0].current && rows[1].account == "a2" && rows[2].signIn);
    CHECK(w.model.tab(w.model.active()).draft.empty());
    // The long email is cut with "…" inside the dialog, never run out of it. Plant: no cut in accountDialog.
    w.frame();
    bool cut = false;
    const std::function<void(PrLayoutNode)> walk = [&](PrLayoutNode n) {
        if (prLayoutNodeKind(w.tree, n) == PrLayoutNodeKind_Text) {
            const auto t = prLayoutNodeText(w.tree, n);
            const std::string text(t.data, t.size);
            if (text.ends_with("\xE2\x80\xA6") && kLong.starts_with(text.substr(0, text.size() - 3))) cut = true;
            CHECK(text != kLong);
        }
        for (PrLayoutNode c = prLayoutNodeFirstChild(w.tree, n); c != kPrLayoutNodeNone; c = prLayoutNodeNextSibling(w.tree, c))
            walk(c);
    };
    walk(prLayoutTreeRoot(w.tree));
    CHECK(cut);
    CHECK(w.switches().empty());
}

// The menu's last row runs /login: its dialog lists the accounts that need signing in. Plant: run /account instead.
void signInRowRunsLogin() {
    Window w;
    w.press(w.centre(w.tabNode("rowan")));
    const PrVec2 last = w.below(w.centre(w.tabNode("rowan")), 2);
    w.slide(last);
    w.lift(last);
    CHECK(w.view.accounts.dialog == AccountMenuView::Dialog::Login);
    CHECK(w.view.accounts.dialogRows.size() == 1 && w.view.accounts.dialogRows[0].account == "a3");
    CHECK(w.switches().empty());
}

// /login of a3: one claim names a3 (token, no request) while rowan's session stays on a1, with no SetAccount first; the
// server signs a3 in and moves the session, and the window sends nothing more. Plants: SetAccount before the claim; a
// second claim or a SetAccount after the sign-in.
void loginSignsInThenSwitches() {
    Window w;
    w.command("/login");
    CHECK(w.view.accounts.dialog == AccountMenuView::Dialog::Login);
    const auto sentBefore = w.link.sent.size();
    w.view.loginAccount("a3");
    CHECK(w.link.sent.size() == sentBefore + 1);
    if (w.link.sent.size() != sentBefore + 1) return;
    const auto& claim = w.link.sent.back();
    CHECK(claim.kind == sync::ActionKind::SignInClaim && claim.account == "a3" && claim.session == kRowan &&
          claim.request == 0 && !claim.text.empty());
    CHECK((w.switches() == std::vector<std::string>{"7:a3"}));
    // The server opens a flow for a3, held by this window; the session stays on a1.
    w.offers = {sync::SignInOffer{.id = 4, .account = "a3", .provider = "claude"}};
    w.publish();
    w.frame();
    CHECK((w.switches() == std::vector<std::string>{"7:a3"}));
    CHECK(!w.view.problem.empty());
    // a3 signs in, and the server moves the session to it.
    w.rowan[2].needsSignIn = false;
    w.offers.clear();
    w.publish();
    w.frame();
    w.current = "a3";
    w.publish();
    w.frame();
    CHECK((w.switches() == std::vector<std::string>{"7:a3"}));
    CHECK(w.view.problem.empty());
}

// A /login whose sign-in ends without signing in (cancelled, failed, expired) changes nothing. Plant: switch anyway.
void cancelledLoginChangesNothing() {
    Window w;
    w.view.loginAccount("a3");
    w.offers = {sync::SignInOffer{.id = 4, .account = "a3", .provider = "claude"}};
    w.publish();
    w.frame();
    w.offers.clear();
    w.publish();    // the flow is gone; a3 still needs signing in
    w.frame();
    CHECK((w.switches() == std::vector<std::string>{"7:a3"}));
    CHECK(w.view.problem.find("did not finish") != std::string::npos);
    CHECK(!w.view.problem.empty());
}

// /logout asks first, naming the email; only the confirm signs out (SignOut naming the session and the account), and
// the AI moves to the next signed-in account by its claim. Plants: sign out on Enter with no confirm; move by SetAccount.
void logoutConfirmsThenMoves() {
    Window w;
    w.command("/logout");
    CHECK(w.view.accounts.dialog == AccountMenuView::Dialog::Logout);
    CHECK(w.view.accounts.logoutShown == "person@home.test");
    CHECK(w.view.accounts.logoutNext == kLong);
    for (const auto& a : w.link.sent) CHECK(a.kind != sync::ActionKind::SignOut);
    CHECK(w.view.logout());
    std::size_t signOut = w.link.sent.size(), move = w.link.sent.size();
    for (std::size_t i = 0; i < w.link.sent.size(); ++i) {
        if (w.link.sent[i].kind == sync::ActionKind::SignOut) signOut = i;
        if (w.link.sent[i].kind == sync::ActionKind::SignInClaim) move = i;
    }
    CHECK(signOut < w.link.sent.size() && w.link.sent[signOut].account == "a1" && w.link.sent[signOut].session == kRowan);
    CHECK(move < w.link.sent.size() && move > signOut && w.link.sent[move].account == "a2" &&
          w.link.sent[move].request == 0);
    CHECK((w.switches() == std::vector<std::string>{"7:a2"}));
}

// /logout of the only signed-in account: the AI is left to the sign-in prompt, with no move. Plant: move anyway.
void logoutOfTheLastAccountMovesNowhere() {
    Window w;
    w.rowan[1].needsSignIn = true;
    w.publish();
    w.frame();
    w.command("/logout");
    CHECK(w.view.accounts.dialog == AccountMenuView::Dialog::Logout && w.view.accounts.logoutNext.empty());
    CHECK(w.view.logout());
    CHECK(w.switches().empty());
}

// The rows from a roster, pure: signed-in accounts only, the current marked, the sign-in row last; and where /logout
// sends the AI.
void rowsFromTheRoster() {
    sync::ServerAI roster{"rowan", {{"a1", "Home", false, "h@x"}, {"+cmd"}, {"a2", "", true}, {"a3", "Lab", false}}};
    const auto rows = accountMenuRows(roster, "a3");
    CHECK(rows.size() == 3 && rows[0].text == "h@x" && !rows[0].current && rows[1].text == "Lab" && rows[1].current &&
          rows[2].signIn);
    CHECK(accountLoginRows(roster).size() == 1 && accountLoginRows(roster)[0].account == "a2");
    CHECK(accountAfterLogout(roster, "a1") == "a3");
    CHECK(accountAfterLogout(roster, "a3") == "a1");
    roster.accounts[3].needsSignIn = true;
    CHECK(accountAfterLogout(roster, "a1").empty());
}

} // namespace

int main() {
    rowsFromTheRoster();
    pressOnTheActiveTabOpensTheMenu();
    pressSlideAndLiftSwitches();
    pressAndLiftClosesAndChangesNothing();
    downThenAcrossStaysTheMenu();
    pressThenSidewaysDragMovesTheTab();
    pressOnAnotherTabNeverOpens();
    theCurrentAccountIsMarked();
    theMenuSitsUnderTheHighlight(PrVec2{1376, 1032}, "iPad 13-inch");
    theMenuSitsUnderTheHighlight(PrVec2{402, 874}, "iPhone 17 Pro");
    theLabelNeverChanges();
    stillFramesWhileTheMenuIsOpen();
    accountCommandOpensTheSelector();
    signInRowRunsLogin();
    loginSignsInThenSwitches();
    cancelledLoginChangesNothing();
    logoutConfirmsThenMoves();
    logoutOfTheLastAccountMovesNowhere();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("account menu: ok");
    return 0;
}
