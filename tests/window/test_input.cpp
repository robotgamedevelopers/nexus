// The window's input, headless: the nexus view built over a Prax UI context and Studio Kit with no window and no GPU,
// fed events as the window would feed them, with the model checked after each. Frames carry a
// made-up clock (from 1000 s), never the real one. Text is set in the system's fonts, so these are functional tests.

#include <cstdio>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_panel.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"
#include "pr/ui_text_edit.h"

#include "test_link_port.h"

#include "model.h"
#include "style.h"
#include "tab_order.h"
#include "view.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

std::string shown(const std::string& s) {
    std::string out = "\"";
    for (char c : s)
        out += c == '\n' ? std::string("\\n") : std::string(1, c);
    return out + "\"";
}

// Events as the window gets them: a key (KeyDown, KeyUp), text (Text, CompositionUpdate, CompositionEnd), a pointer at
// `at`, and a scroll by `delta` px at `at`.
PrAppEvent keyEvent(PrAppEventKind kind, PrAppKey key, PrAppKeyCode code = 0, PrAppModifierFlags modifiers = 0) {
    PrAppEvent e{};
    e.kind = kind;
    e.modifiers = modifiers;
    e.keyboard.key = key;
    e.keyboard.code = code;
    return e;
}

PrAppEvent textEvent(PrAppEventKind kind, PrStringView text, PrAppModifierFlags modifiers = 0) {
    PrAppEvent e{};
    e.kind = kind;
    e.modifiers = modifiers;
    e.input.text = text;
    return e;
}

PrAppEvent pointerEvent(PrAppEventKind kind, PrVec2 at) {
    PrAppEvent e{};
    e.kind = kind;
    e.pointer.position = at;
    return e;
}

PrAppEvent scrollEvent(PrVec2 at, PrVec2 delta) {
    PrAppEvent e{};
    e.kind = PrAppEventKind_Scroll;
    e.scroll.position = at;
    e.scroll.delta = delta;
    e.scroll.unit = PrAppScrollUnit_Pixel;
    return e;
}

std::vector<FriendInfo> friendsNamed(std::size_t count) {
    std::vector<FriendInfo> out;
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(FriendInfo{"f" + std::to_string(i + 1), "m"});
    return out;
}

// The terminal tabs' shells with no process: it records what the view asks of it.
struct FakeTerminals final : Terminals {
    std::set<std::size_t> running;              // opened and not closed
    std::vector<std::size_t> closed;            // in the order closed
    std::vector<std::size_t> shown;             // the ids shown, one per round
    std::map<std::size_t, int> updates;         // rounds each was serviced without showing
    std::set<std::size_t> finished;             // ids whose shell has ended
    std::map<std::size_t, std::string> titles;
    PrLayoutNode parent = kPrLayoutNodeNone;    // the parent the last show was given
    float scale = 1;
    bool refuse = false;                        // open fails

    bool open(std::size_t id) override {
        if (refuse)
            return false;
        running.insert(id);
        return true;
    }
    void close(std::size_t id) override {
        running.erase(id);
        closed.push_back(id);
    }
    void show(std::size_t id, PrLayoutNode p) override {
        shown.push_back(id);
        parent = p;
    }
    void update(std::size_t id) override { ++updates[id]; }
    bool ended(std::size_t id) override { return finished.count(id) != 0; }
    std::map<std::size_t, std::string> problems;    // tabs whose shell cannot run here (on Windows: no WSL2)
    std::string problem(std::size_t id) override { return problems.count(id) ? problems[id] : std::string(); }
    std::string title(std::size_t id) override { return titles.count(id) ? titles[id] : std::string(); }
    void setTextScale(float k) override { scale = k; }
};

// The sprint tab's page with no page: it records what the view asks of it, and shows a page or gives `problem`.
struct FakeBoard final : Board {
    bool page = true;                           // show() declares a page; otherwise it gives `problem`
    std::string problem = "the board is down; run `nova-sprint up`";
    int shows = 0;                              // rounds the sprint tab showed
    int hiddens = 0;                            // rounds it did not
    PrVec2 extent{};                            // the extent the last show was given
    float scale = 1;
    bool ready = true;
    bool prepared() override { return ready; }
    int homes = 0;                              // times the sprint tab was chosen (Board::home)

    bool show(PrLayoutNode, PrVec2 e, std::string* why) override {
        ++shows;
        extent = e;
        if (!page)
            *why = problem;
        return page;
    }
    void hidden() override { ++hiddens; }
    void setTextScale(float k) override { scale = k; }
    void home() override { ++homes; }
};

// A window with no window: the view, its model of two friends (or as many as asked), and a kit over a UI context over
// a layout tree. With `sprint`, the sprint tab stands first, as in the app.
struct Harness {
    Model model;
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    Look look;
    FakeTerminals terminals;
    FakeBoard board;
    View view;
    PrAppDisplay display = prAppDisplayDefault();
    double time = 1000;
    PrVec2 extent{1000, 780};
    int hides = 0;      // hide requests reaching the host seam (View::hide), which the overlay connects to hideOverlay
    int stops = 0;      // stop requests (View::stop)
    int saves = 0;      // tab order saves asked for after a move (View::moved)
    uint32_t rounds = 8; // the most rounds a frame runs

    Harness() : Harness(std::vector<FriendInfo>{FriendInfo{"alex", "m1"}, FriendInfo{"emma", "m2"}}) {}

    // With `overlay`, the window is drawn as the see-through overlay is.
    explicit Harness(const std::vector<FriendInfo>& friends, bool overlay = false, bool sprint = false)
            : model(friends, sprint) {
        prFontSetSetSystem(fonts, system);
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
        PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        look.overlay = overlay;
        PrKitTheme theme = windowTheme(fonts, overlay);
        kit = prKitCreate(ui, &theme);
        view.model = &model;
        view.look = &look;
        view.terminals = &terminals;
        view.board = &board;
        view.extent = PrVec2{1000, 780};
        view.hide = [this] { ++hides; };
        view.stop = [this] { ++stops; };
        view.moved = [this] { ++saves; };
        frame();
        frame();
    }

    ~Harness() {
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prFontSystemDestroy(system);
    }

    void frame() {
        beginWindowFrame(view, kit, fonts);
        time += 1.0 / 60;
        view.time = time;   // as the window's beginFrame gives it the frame's time
        PrUiFrameDesc desc{
            .time = time,
            .delta = 1.0 / 60,
            .extent = extent,
            .maxRounds = rounds,
            .display = display,
            .preferences = {},
        };
        // Under a surface that fills the viewport, as Prax's studio window builds the app's round (studio_window.cpp
        // build), so that the window is not the tree's root, as in the app.
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Harness& h = *static_cast<Harness*>(context);
            PrKitPanelStyle style{
                .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
                .background = PrKitBackground(h.look.overlay ? PrKitBackground_None : PrKitBackground_App),
            };
            buildWindow(h.view, h.kit, prKitPanel(h.kit, kPrLayoutNodeNone, {}, style));
        }, this);
    }

    // Queues an event as the window does: to the context's main viewport, as it arrives (Caps Lock and all).
    void push(PrAppEvent e) {
        e.time = time;
        prUiPushEvent(ui, kPrUiViewportMain, &e);
    }

    void key(PrAppKey k, PrAppKeyCode code, PrAppModifierFlags modifiers = 0, const char* text = nullptr) {
        PrStringView typed = text != nullptr ? PrStringView{text, std::char_traits<char>::length(text)} : PrStringView{};
        push(keyEvent(PrAppEventKind_KeyDown, k, code, modifiers));
        if (typed.size != 0)
            push(textEvent(PrAppEventKind_Text, typed, modifiers));
        push(keyEvent(PrAppEventKind_KeyUp, k, code, modifiers));
        frame();
    }

    void type(const char* text, PrAppModifierFlags modifiers = 0) {
        for (const char* c = text; *c != 0; ++c) {
            push(keyEvent(PrAppEventKind_KeyDown, PrAppKey(*c), 0, modifiers));
            push(textEvent(PrAppEventKind_Text, PrStringView{c, 1}, modifiers));
            push(keyEvent(PrAppEventKind_KeyUp, PrAppKey(*c), 0, modifiers));
            frame();
        }
    }

    void enter(PrAppModifierFlags modifiers = 0) {
        key(PrAppKey_Enter, PrAppKeyCode_Enter, modifiers);
    }

    // Command-digit as macOS sends it: the digit as the key, its code, and no text (a Command chord types none).
    void commandDigit(int digit, PrAppModifierFlags extra = 0) {
        PrAppKeyCode code = digit == 0 ? PrAppKeyCode_Digit0 : PrAppKeyCode(PrAppKeyCode_Digit1 + digit - 1);
        key(PrAppKey('0' + digit), code, PrAppModifierFlags(style::kCommand | extra));
    }

    const Tab& active() const {
        return model.tab(model.active());
    }

    // The transcript's entries, at the last layout: the children of its column.
    std::vector<PrLayoutNode> entries() const {
        std::vector<PrLayoutNode> out;
        PrLayoutNode column = view.conversation ? view.conversation : prLayoutNodeFirstChild(tree, view.transcript);
        for (PrLayoutNode n = prLayoutNodeFirstChild(tree, column); n != kPrLayoutNodeNone;
                n = prLayoutNodeNextSibling(tree, n))
            out.push_back(n);
        return out;
    }

    PrRect bounds(PrLayoutNode node) const {
        return prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    }

    float height(PrLayoutNode node) const {
        PrRect b = bounds(node);
        return b.bottom - b.top;
    }

    // The height of the prompt's text, and of the transcript's first entry, at the last layout.
    float promptTextHeight() const {
        return height(prLayoutNodeFirstChild(tree, view.prompt));
    }
    float firstEntryHeight() const {
        std::vector<PrLayoutNode> e = entries();
        return e.empty() ? 0 : height(e.front());
    }

    // The tab strip, the window's first child, and its tabs in order, at the last layout. Outside a drag the strip holds
    // only its tabs.
    PrLayoutNode strip() const {
        return prLayoutNodeFirstChild(tree, window());
    }
    // The window: the surface's one child (frame).
    PrLayoutNode window() const {
        return prLayoutNodeFirstChild(tree, prLayoutTreeRoot(tree));
    }
    // The native tab nodes from the last layout, in model order across all strips.
    std::vector<PrLayoutNode> tabs() const { return view.tabNodes; }

    // The actual text leaf, which inherits the Prax label's foreground.
    PrLayoutNode findText(PrLayoutNode node, const std::string& text) const {
        if (node == kPrLayoutNodeNone) return kPrLayoutNodeNone;
        if (prLayoutNodeKind(tree, node) == PrLayoutNodeKind_Text) {
            const PrStringView t = prLayoutNodeText(tree, node);
            return t.data && std::string_view(t.data, t.size) == text ? node : kPrLayoutNodeNone;
        }
        for (PrLayoutNode n = prLayoutNodeFirstChild(tree, node); n != kPrLayoutNodeNone;
             n = prLayoutNodeNextSibling(tree, n)) {
            const auto found = findText(n, text);
            if (found != kPrLayoutNodeNone) return found;
        }
        return kPrLayoutNodeNone;
    }
    bool hasText(PrLayoutNode node, const std::string& text) const { return findText(node, text) != kPrLayoutNodeNone; }
    PrVec2 centre(PrLayoutNode node) const {
        PrRect b = bounds(node);
        return PrVec2{(b.left + b.right) / 2, (b.top + b.bottom) / 2};
    }

    void pointer(PrAppEventKind kind, PrVec2 at) {
        push(pointerEvent(kind, at));
        frame();
    }

    // Presses the tab at `from`, drags it past the kit's threshold and on over the tab at `to`, near its far edge (its
    // right edge moving right, its left edge moving left, so that it lands at `to`), and, if `release`, releases it.
    void dragTab(std::size_t from, std::size_t to, bool release = true) {
        std::vector<PrLayoutNode> t = tabs();
        PrVec2 start = centre(t[from]);
        PrVec2 end{to > from ? bounds(t[to]).right - 2 : bounds(t[to]).left + 2, start.y};
        pointer(PrAppEventKind_PointerDown, start);
        pointer(PrAppEventKind_PointerMove, PrVec2{start.x + (end.x < start.x ? -10.0f : 10.0f), start.y});
        pointer(PrAppEventKind_PointerMove, end);
        if (release)
            pointer(PrAppEventKind_PointerUp, end);
    }

    // The friends' names in tab order.
    std::string order() const {
        std::string out;
        for (std::size_t j = 0; j < model.tab_count(); ++j)
            out += (j ? " " : "") + model.tab(j).name;
        return out;
    }

    // The tabs' ids in order.
    std::string ids() const {
        std::string out;
        for (std::size_t j = 0; j < model.tab_count(); ++j)
            out += (j ? " " : "") + std::to_string(model.tab(j).id);
        return out;
    }

    // Command-T and Command-W, as macOS sends them: the letter's key, its code, and no text.
    void commandT() {
        key(PrAppKey('T'), PrAppKeyCode_KeyT, style::kCommand);
    }
    void commandW() {
        key(PrAppKey('W'), PrAppKeyCode_KeyW, style::kCommand);
    }

    // The tab labels' texts, in order.
    std::vector<std::string> labels() const {
        std::vector<std::string> out;
        for (PrLayoutNode t : tabs()) {
            PrLayoutNode only = prLayoutNodeFirstChild(tree, t);
            while (only != kPrLayoutNodeNone && prLayoutNodeKind(tree, only) != PrLayoutNodeKind_Text)
                only = prLayoutNodeFirstChild(tree, only);
            PrStringView text = only != kPrLayoutNodeNone ? prLayoutNodeText(tree, only) : PrStringView{};
            out.emplace_back(text.data != nullptr ? text.data : "", text.size);
        }
        return out;
    }

    // Command-= (larger), Command-+ (Shift-=, larger) and Command-minus (smaller), as macOS sends them: no text. Then a
    // frame more, whose start applies the scale.
    void larger(PrAppModifierFlags extra = 0) {
        key(PrAppKey('='), PrAppKeyCode_Equal, PrAppModifierFlags(style::kCommand | extra));
        frame();
    }
    void largerWithShift() {
        key(PrAppKey('+'), PrAppKeyCode_Equal, PrAppModifierFlags(style::kCommand | PrAppModifierFlag_Shift));
        frame();
    }
    void smaller(PrAppModifierFlags extra = 0) {
        key(PrAppKey('-'), PrAppKeyCode_Minus, PrAppModifierFlags(style::kCommand | extra));
        frame();
    }
};

void typingFillsTheActiveDraft() {
    Harness h;
    h.type("hello");
    CHECK(h.active().draft == "hello");
    CHECK(h.model.tab(1).draft.empty());
}

void enterSendsExactlyTheText() {
    Harness h;
    h.type("hello");
    h.enter();
    if (h.active().transcript.size() == 1 && h.active().transcript[0].text != "hello")
        std::fprintf(stderr, "  sent %s\n", shown(h.active().transcript[0].text).c_str());
    CHECK(h.active().transcript.size() == 1);
    CHECK(h.active().transcript.size() == 1 && h.active().transcript[0].kind == EntryKind::Person);
    CHECK(h.active().transcript.size() == 1 && h.active().transcript[0].text == "hello");
    if (!h.active().draft.empty())
        std::fprintf(stderr, "  draft after send %s\n", shown(h.active().draft).c_str());
    CHECK(h.active().draft.empty());
    // The prompt is empty after the send, and the next message starts clean.
    h.type("again");
    CHECK(h.active().draft == "again");
}

void logoutRequiresAnExplicitEnterAndNeverBecomesChat() {
    Harness h;
    int commands = 0;
    h.view.signOut = [&] { ++commands; return true; };
    h.type("/logout");
    h.enter(style::kCommand);
    CHECK(commands == 0 && h.active().queue.empty() && h.active().transcript.empty());
    CHECK(h.active().draft == "/logout");
    h.view.activateSignIn = [] { return true; };
    h.enter();
    CHECK(commands == 1 && h.active().draft.empty() && h.active().transcript.empty());
    h.view.activateSignIn = {};
    h.type("Explain /logout");
    h.enter();
    CHECK(commands == 1 && h.active().transcript.size() == 1);
    CHECK(h.active().transcript.front().text == "Explain /logout");

    Harness offline;
    offline.type("/logout");
    offline.enter();
    CHECK(offline.active().transcript.empty() && offline.active().queue.empty() && !offline.view.problem.empty());
}

void enterStartsSignInBeforeEditingTheDraft() {
    Harness h;
    bool needed = true;
    int attempts = 0;
    h.view.activateSignIn = [&] { ++attempts; return needed; };
    h.enter();
    CHECK(attempts == 1 && h.active().draft.empty() && h.active().transcript.empty());
    h.type("  ready after login  ");
    h.enter();
    CHECK(attempts == 2 && h.active().draft == "  ready after login  " && h.active().transcript.empty());
    needed = false;
    h.enter();
    CHECK(attempts == 3 && h.active().draft.empty());
    CHECK(h.active().transcript.size() == 1 && h.active().transcript.front().text == "  ready after login  ");
}

void commandEnterQueues() {
    Harness h;
    h.type("later");
    h.enter(style::kCommand);
    CHECK((h.active().queue == std::vector<std::string>{"later"}));
    CHECK(h.active().transcript.empty());     // the queue, not the transcript
    CHECK(h.active().draft.empty());
    CHECK(h.model.tab(1).queue.empty());
    h.type("then this");
    h.enter(style::kCommand);
    CHECK((h.active().queue == std::vector<std::string>{"later", "then this"}));
    CHECK(h.active().transcript.empty());
}

void shiftEnterInsertsOneLineFeed() {
    Harness h;
    h.type("a");
    h.enter(PrAppModifierFlag_Shift);
    h.type("b");
    if (h.active().draft != "a\nb")
        std::fprintf(stderr, "  draft %s\n", shown(h.active().draft).c_str());
    CHECK(h.active().draft == "a\nb");
    CHECK(h.active().transcript.empty());
    // The prompt grows to the two lines, and back to one once they are sent.
    h.frame();
    CHECK(h.view.promptRows == 2);
    h.enter();
    h.frame();
    CHECK(h.view.promptRows == 1);
}

void blankDraftsSendNothing() {
    Harness h;
    h.enter();
    CHECK(h.active().transcript.empty());
    h.type("   ");
    h.enter();
    h.enter(style::kCommand);
    CHECK(h.active().transcript.empty());
    CHECK(h.active().queue.empty());
    CHECK(h.active().draft == "   ");
}

void commandDigitSelectsATabAndTypesNothing() {
    Harness h;
    h.commandDigit(2);
    CHECK(h.model.active() == 1);
    h.commandDigit(1);
    CHECK(h.model.active() == 0);
    for (int d : {3, 4, 5, 6, 7, 8, 9, 0}) {   // no such tab: nothing changes
        h.commandDigit(d);
        CHECK(h.model.active() == 0);
    }
    CHECK(h.model.tab(0).draft.empty());
    CHECK(h.model.tab(1).draft.empty());
}

void commandZeroSelectsTheTenthTab() {
    Harness ten(friendsNamed(10));
    ten.commandDigit(0);
    CHECK(ten.model.active() == 9);
    ten.commandDigit(9);
    CHECK(ten.model.active() == 8);
    Harness eleven(friendsNamed(11));
    eleven.commandDigit(0);
    CHECK(eleven.model.active() == 9);   // the eleventh tab has no key
    Harness nine(friendsNamed(9));
    nine.commandDigit(9);
    CHECK(nine.model.active() == 8);
    nine.commandDigit(0);
    CHECK(nine.model.active() == 8);     // no tenth tab: nothing changes
    for (std::size_t j = 0; j < nine.model.tab_count(); ++j)
        CHECK(nine.model.tab(j).draft.empty());
}

void capsLockKeepsTheBindings() {
    Harness h;
    h.type("hi", PrAppModifierFlag_CapsLock);
    h.enter(PrAppModifierFlag_CapsLock);
    CHECK(h.active().transcript.size() == 1 && h.active().transcript[0].text == "hi");
    h.type("x", PrAppModifierFlag_CapsLock);
    h.enter(PrAppModifierFlags(style::kCommand | PrAppModifierFlag_CapsLock));
    CHECK(h.active().transcript.size() == 1);
    CHECK((h.active().queue == std::vector<std::string>{"x"}));
    h.commandDigit(2, PrAppModifierFlag_CapsLock);
    CHECK(h.model.active() == 1);
    h.commandDigit(1, PrAppModifierFlag_CapsLock);
    CHECK(h.model.active() == 0);
}

void eachTabKeepsItsDraft() {
    Harness h;
    h.type("one");
    h.commandDigit(2);
    h.type("two");
    h.commandDigit(1);
    CHECK(h.model.tab(0).draft == "one");
    CHECK(h.model.tab(1).draft == "two");
    h.type("!");            // the caret comes back at the draft's end
    CHECK(h.model.tab(0).draft == "one!");
    h.commandDigit(2);
    h.enter();
    CHECK(h.model.tab(1).transcript.size() == 1 && h.model.tab(1).transcript[0].text == "two");
    CHECK(h.model.tab(0).transcript.empty());
    CHECK(h.model.tab(0).draft == "one!");
}

void enterWhileComposingCommitsOnly() {
    Harness h;
    const char* kana = "\xE3\x81\x8B";
    h.push(keyEvent(PrAppEventKind_KeyDown, 'K', PrAppKeyCode_KeyK));
    h.push(textEvent(PrAppEventKind_CompositionUpdate, PrStringView{kana, 3}));
    h.push(keyEvent(PrAppEventKind_KeyUp, 'K', PrAppKeyCode_KeyK));
    h.frame();
    // macOS sends the KeyDown first, then the input method's commit: CompositionEnd, then the text.
    h.push(keyEvent(PrAppEventKind_KeyDown, PrAppKey_Enter, PrAppKeyCode_Enter));
    h.push(textEvent(PrAppEventKind_CompositionEnd, PrStringView{kana, 3}));
    h.push(textEvent(PrAppEventKind_Text, PrStringView{kana, 3}));
    h.push(keyEvent(PrAppEventKind_KeyUp, PrAppKey_Enter, PrAppKeyCode_Enter));
    h.frame();
    CHECK(h.active().transcript.empty());
    CHECK(h.active().draft == kana);
    h.enter();
    CHECK(h.active().transcript.size() == 1 && h.active().transcript[0].text == kana);
}

bool near(float a, float b) {
    return a > b - 0.5f && a < b + 0.5f;
}

void textKeysScaleTheTranscriptAndThePrompt() {
    Harness h;
    h.model.append(0, Entry{EntryKind::Friend, "a reply"});
    h.type("hello");
    h.frame();
    const float prompt1 = h.promptTextHeight();
    const float entry1 = h.firstEntryHeight();
    CHECK(prompt1 > 0 && entry1 > 0);
    CHECK(h.look.textSteps == 0);

    h.larger();
    CHECK(h.look.textSteps == 1);
    CHECK(h.promptTextHeight() > prompt1 + 1);
    CHECK(h.firstEntryHeight() > entry1 + 1);
    h.smaller();
    CHECK(h.look.textSteps == 0);
    CHECK(near(h.promptTextHeight(), prompt1));
    CHECK(near(h.firstEntryHeight(), entry1));
    h.largerWithShift();
    CHECK(h.look.textSteps == 1);
    h.smaller();
    h.smaller();
    CHECK(h.look.textSteps == -1);
    CHECK(h.promptTextHeight() < prompt1 - 1);
    CHECK(h.firstEntryHeight() < entry1 - 1);

    // Neither key types a character, and the draft and transcript are as they were.
    CHECK(h.active().draft == "hello");
    CHECK(h.active().transcript.size() == 1);
}

void textScaleIsClamped() {
    Harness h;
    for (int i = 0; i < style::kTextStepsUp + 5; ++i)
        h.larger();
    CHECK(h.look.textSteps == style::kTextStepsUp);
    CHECK(near(style::textScale(h.look.textSteps), style::kTextScaleMax));
    for (int i = 0; i < style::kTextStepsUp + style::kTextStepsDown + 5; ++i)
        h.smaller();
    CHECK(h.look.textSteps == -style::kTextStepsDown);
    CHECK(near(style::textScale(h.look.textSteps), style::kTextScaleMin));
    CHECK(h.active().draft.empty());
}

// Each text scale applied, and only a change, asks for it to be saved, with the scale applied; a scale restored at
// launch is applied at the first frame, terminals and page too, and saved like any other.
void textScaleChangesAreSaved() {
    Harness h;
    std::vector<int> saved;
    h.view.scaled = [&h, &saved] { saved.push_back(h.look.textSteps); };
    h.larger();
    h.larger();
    h.smaller();
    CHECK((saved == std::vector<int>{1, 2, 1}));
    h.frame();
    CHECK(saved.size() == 3);
    for (int i = 0; i < style::kTextStepsUp + 5; ++i)
        h.larger();
    CHECK(saved.back() == style::kTextStepsUp && saved.size() == 3 + std::size_t(style::kTextStepsUp - 1));
    Harness relaunched;
    std::vector<int> again;
    relaunched.view.scaled = [&relaunched, &again] { again.push_back(relaunched.look.textSteps); };
    relaunched.view.askedTextSteps = 3;     // as main.cpp sets it from the saved file
    relaunched.frame();
    CHECK(relaunched.look.textSteps == 3 && (again == std::vector<int>{3}));
    CHECK(near(relaunched.terminals.scale, style::textScale(3)) && near(relaunched.board.scale, style::textScale(3)));
}

void textKeysWorkWithCapsLock() {
    Harness h;
    h.larger(PrAppModifierFlag_CapsLock);
    CHECK(h.look.textSteps == 1);
    h.smaller(PrAppModifierFlag_CapsLock);
    h.smaller(PrAppModifierFlag_CapsLock);
    CHECK(h.look.textSteps == -1);
    CHECK(h.active().draft.empty());
}

// Escape asks for the overlay to hide and does not stop; Command-Escape stops and does not hide. With Caps Lock on, both
// still do. Neither types anything or changes a draft, transcript, queue or the active tab.
void escapeHidesAndCommandEscapeStops() {
    Harness h;
    h.type("draft");
    h.enter(style::kCommand);
    h.type("more");
    h.model.append(0, Entry{EntryKind::Friend, "a reply"});
    h.commandDigit(2);
    h.type("other");
    h.commandDigit(1);
    const Tab before0 = h.model.tab(0);
    const Tab before1 = h.model.tab(1);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    CHECK(h.hides == 1 && h.stops == 0);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, PrAppModifierFlag_CapsLock);
    CHECK(h.hides == 2 && h.stops == 0);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, style::kCommand);
    CHECK(h.hides == 2 && h.stops == 1);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, PrAppModifierFlags(style::kCommand | PrAppModifierFlag_CapsLock));
    CHECK(h.hides == 2 && h.stops == 2);
    CHECK(h.model.active() == 0);
    for (std::size_t i = 0; i < 2; ++i) {
        const Tab& was = i == 0 ? before0 : before1;
        const Tab& now = h.model.tab(i);
        CHECK(now.draft == was.draft);
        CHECK(now.queue == was.queue);
        CHECK(now.transcript.size() == was.transcript.size());
    }
    CHECK(h.model.tab(0).draft == "more");
    CHECK((h.model.tab(0).queue == std::vector<std::string>{"draft"}));
    // The prompt still has focus and takes typing after them.
    h.type("!");
    CHECK(h.model.tab(0).draft == "more!");
}

// Windowed, nothing is connected to hide: Escape requests no hide and changes nothing.
void escapeWindowedHidesNothing() {
    Harness h;
    h.view.hide = nullptr;
    h.type("draft");
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    CHECK(h.hides == 0 && h.stops == 0);
    CHECK(h.active().draft == "draft");
    CHECK(h.model.active() == 0);
}

// While an input method composes, Escape is the input method's (it cancels the composition): no hide.
void escapeWhileComposingDoesNotHide() {
    Harness h;
    const char* kana = "\xE3\x81\x8B";
    h.push(keyEvent(PrAppEventKind_KeyDown, 'K', PrAppKeyCode_KeyK));
    h.push(textEvent(PrAppEventKind_CompositionUpdate, PrStringView{kana, 3}));
    h.push(keyEvent(PrAppEventKind_KeyUp, 'K', PrAppKeyCode_KeyK));
    h.frame();
    // macOS sends the KeyDown first, then the input method's cancel: CompositionEnd with no text.
    h.push(keyEvent(PrAppEventKind_KeyDown, PrAppKey_Escape, PrAppKeyCode_Escape));
    h.push(textEvent(PrAppEventKind_CompositionEnd, PrStringView{}));
    h.push(keyEvent(PrAppEventKind_KeyUp, PrAppKey_Escape, PrAppKeyCode_Escape));
    h.frame();
    CHECK(h.hides == 0);
    CHECK(h.active().draft.empty());
    // Composition over, Escape hides again.
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    CHECK(h.hides == 1);
}

// Four friends: f1 f2 f3 f4.
std::vector<FriendInfo> fourFriends() {
    return friendsNamed(4);
}

// A drag of the tab at position 3 to position 1 reorders the strip, keeps the active friend, and Command-1 then selects
// the moved tab. Each tab shows its friend's name only.
void dragMovesATab() {
    Harness h(fourFriends());
    h.commandDigit(2);                       // f2 active
    CHECK(h.model.active() == 1);
    CHECK(h.saves == 0);
    h.dragTab(2, 0);
    CHECK(h.order() == "f3 f1 f2 f4");
    CHECK(h.saves == 1);                     // a move asks for the order to be saved, once
    CHECK(h.active().name == "f2");          // the same friend, now at position 3
    CHECK(h.model.active() == 2);
    CHECK(h.hides == 0);
    h.frame();
    std::vector<PrLayoutNode> t = h.tabs();
    CHECK(t.size() == 4);
    for (std::size_t j = 0; j < t.size() && j < h.model.tab_count(); ++j) {
        PrLayoutNode only = prLayoutNodeFirstChild(h.tree, t[j]);
        CHECK(only != kPrLayoutNodeNone && prLayoutNodeNextSibling(h.tree, only) == kPrLayoutNodeNone);
        CHECK(h.hasText(t[j], h.model.tab(j).name));
    }
    // The tabs stand left to right in the new order.
    CHECK(t.size() == 4 && h.bounds(t[0]).right <= h.bounds(t[1]).left + 0.5f);
    h.commandDigit(1);
    CHECK(h.active().name == "f3");
    h.commandDigit(3);
    CHECK(h.active().name == "f2");
    // And back to the end.
    h.dragTab(0, 3);
    CHECK(h.order() == "f1 f2 f4 f3");
    CHECK(h.active().name == "f2");
}

// A press and release that does not pass the threshold is a click: it selects the tab and moves nothing.
void clickBelowThresholdSelects() {
    Harness h(fourFriends());
    PrVec2 at = h.centre(h.tabs()[2]);
    h.pointer(PrAppEventKind_PointerDown, at);
    h.pointer(PrAppEventKind_PointerMove, PrVec2{at.x + 2, at.y});
    h.pointer(PrAppEventKind_PointerUp, PrVec2{at.x + 2, at.y});
    h.frame();
    CHECK(h.order() == "f1 f2 f3 f4");
    CHECK(h.model.active() == 2);
    CHECK(h.saves == 0);                     // a click moves nothing and saves nothing
}

// A click without any movement selects the tab.
void clickSelectsATab() {
    Harness h(fourFriends());
    PrVec2 at = h.centre(h.tabs()[3]);
    h.pointer(PrAppEventKind_PointerDown, at);
    h.pointer(PrAppEventKind_PointerUp, at);
    h.frame();
    CHECK(h.model.active() == 3);
    CHECK(h.order() == "f1 f2 f3 f4");
    CHECK(h.saves == 0);
}

// A relaunch: the friends come in the saved order (tab_order.h), so Command-1 selects the first saved friend.
void savedOrderOnRelaunch() {
    std::vector<FriendInfo> saved = applyTabOrder(fourFriends(), {"f3", "f1"});
    Harness h(saved);
    CHECK(h.order() == "f3 f1 f2 f4");
    h.commandDigit(2);
    h.commandDigit(1);
    CHECK(h.active().name == "f3");
}

// Escape during a drag cancels it: the order stays, the release drops nothing, and no hide is requested.
void escapeCancelsADrag() {
    Harness h(fourFriends());
    h.dragTab(3, 0, false);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    PrVec2 end{h.bounds(h.tabs()[0]).left + 2, h.centre(h.tabs()[0]).y};
    h.pointer(PrAppEventKind_PointerUp, end);
    h.frame();
    CHECK(h.order() == "f1 f2 f3 f4");
    CHECK(h.hides == 0);
    CHECK(h.saves == 0);                     // a cancelled drag saves nothing
    CHECK(h.model.active() == 0);
    // After the drag, Escape hides again.
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    CHECK(h.hides == 1);
}

// Each friend's unsent text and transcript scroll go with its tab, not its position.
void draftAndScrollStayWithTheirFriend() {
    Harness h(fourFriends());
    for (int i = 0; i < 40; ++i)
        h.model.append(0, Entry{EntryKind::Friend, "line " + std::to_string(i)});
    h.frame();
    h.frame();
    h.type("for f1");
    PrRect port = h.bounds(h.view.transcript);
    PrVec2 middle{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
    h.push(pointerEvent(PrAppEventKind_PointerMove, middle));
    h.push(scrollEvent(middle, PrVec2{0, -400}));
    h.frame();
    h.frame();
    const float scrolled = prLayoutUnitToPx(prLayoutNodeScroll(h.tree, h.view.transcript).y);
    CHECK(scrolled < -100 || scrolled > 100);
    // In mid-history the content extends past both edges. The viewport clips just the two bottom corners.
    auto hitsConversation = [&](float x, float y) {
        PrLayoutNode hits[64]{};
        auto count = prLayoutTreeHitTest(h.tree, {prLayoutUnitFromPx(x), prLayoutUnitFromPx(y)}, hits, 64);
        return std::find(hits, hits + std::min(count, 64u), h.view.conversation) != hits + std::min(count, 64u);
    };
    CHECK(hitsConversation(port.left + 1, port.top + 1));
    CHECK(hitsConversation(port.right - 2, port.top + 1));
    CHECK(hitsConversation(middle.x, port.bottom - 2));
    CHECK(!hitsConversation(port.left + 1, port.bottom - 2));
    CHECK(!hitsConversation(port.right - 2, port.bottom - 2));
    h.commandDigit(2);
    h.type("for f2");
    h.dragTab(0, 3);                         // f1 to the end
    CHECK(h.order() == "f2 f3 f4 f1");
    CHECK(h.active().name == "f2" && h.active().draft == "for f2");
    // f2, never scrolled, shows its end (with no entries, the top), as laid out.
    PrLayoutScrollInfo info{};
    CHECK(prLayoutNodeScrollInfo(h.tree, h.view.transcript, &info) && info.offset.y.raw == info.range.bottom.raw);
    CHECK(info.offset.y.raw == 0);
    h.commandDigit(4);
    h.frame();
    CHECK(h.active().name == "f1" && h.active().draft == "for f1");
    const float back = prLayoutUnitToPx(prLayoutNodeScroll(h.tree, h.view.transcript).y);
    CHECK(back > scrolled - 0.5f && back < scrolled + 0.5f);
    h.type("!");                             // the prompt edits f1's draft, at its end
    CHECK(h.active().draft == "for f1!");
    const auto position = [&] {
        PrLayoutScrollInfo result{};
        CHECK(prLayoutNodeScrollInfo(h.tree, h.view.transcript, &result));
        return result;
    };
    h.key(PrAppKey_Up, PrAppKeyCode_ArrowUp, PrAppModifierFlag_Control);
    CHECK(position().offset.y.raw == position().range.top.raw);
    h.key(PrAppKey_PageDown, PrAppKeyCode_PageDown, 0);
    CHECK(position().offset.y.raw > position().range.top.raw);
    h.key(PrAppKey_PageUp, PrAppKeyCode_PageUp, 0);
    CHECK(position().offset.y.raw == position().range.top.raw);
    h.key(PrAppKey_Down, PrAppKeyCode_ArrowDown, PrAppModifierFlag_Control);
    CHECK(position().offset.y.raw == position().range.bottom.raw);
    CHECK(h.model.tab(0).draft == "for f2");
}

// With one tab there is nowhere to move it: a drag changes nothing.
void dragWithOneTabDoesNothing() {
    Harness h(friendsNamed(1));
    h.type("x");
    h.dragTab(0, 0);
    PrRect b = h.bounds(h.tabs()[0]);
    h.pointer(PrAppEventKind_PointerDown, h.centre(h.tabs()[0]));
    h.pointer(PrAppEventKind_PointerMove, PrVec2{b.right + 200, (b.top + b.bottom) / 2});
    h.pointer(PrAppEventKind_PointerUp, PrVec2{b.right + 200, (b.top + b.bottom) / 2});
    CHECK(h.order() == "f1");
    CHECK(h.model.active() == 0);
    CHECK(h.active().draft == "x");
    CHECK(h.saves == 0);
}

// All text under `node`, with each text node's text on its own.
void textsUnder(const Harness& h, PrLayoutNode node, std::vector<std::string>& out) {
    if (prLayoutNodeKind(h.tree, node) == PrLayoutNodeKind_Text) {
        PrStringView t = prLayoutNodeText(h.tree, node);
        out.emplace_back(t.data, t.size);
    }
    for (PrLayoutNode c = prLayoutNodeFirstChild(h.tree, node); c != kPrLayoutNodeNone; c = prLayoutNodeNextSibling(h.tree, c))
        textsUnder(h, c, out);
}

// The empty prompt shows no text (no placeholder), and an empty tab's transcript shows nothing (no hint). The prompt's
// one text node is the edit's own: Prax shows an empty value as a single space, so the caret has a line
// (ui_text_edit.cpp, "a space after it if it is empty").
void insertionCaretAndPromptMargins() {
    Harness h;
    h.type("Wide iii");
    h.frame();
    const auto width = [&] {
        const auto rect = h.bounds(prLayoutNodeNextSibling(h.tree, h.view.prompt));
        return rect.right - rect.left;
    };
    const auto block = width();
    h.key(PrAppKey_Left, PrAppKeyCode_ArrowLeft);
    h.frame();
    const auto thin = width();
    CHECK(thin >= 2 && thin < block);
    for (int i = 0; i < 7; ++i) {
        h.key(PrAppKey_Left, PrAppKeyCode_ArrowLeft);
        h.frame();
        CHECK(std::abs(width() - thin) < 0.1f);
    }
    for (int i = 0; i < 8; ++i) {
        h.key(PrAppKey_Right, PrAppKeyCode_ArrowRight);
        h.frame();
        CHECK(std::abs(width() - (i == 7 ? block : thin)) < 0.1f);
    }
    const auto panel = h.bounds(h.view.promptBox);
    const auto from = PrVec2{panel.right - 4, panel.top + 1};
    const auto to = PrVec2{panel.left + 4, panel.bottom - 1};
    const auto pointer = [&](PrAppEventKind kind, PrVec2 pos) {
        const auto event = pointerEvent(kind, pos);
        if (!routePromptPointer(h.view, h.ui, event)) h.push(event);
        h.frame();
    };
    pointer(PrAppEventKind_PointerDown, from);
    pointer(PrAppEventKind_PointerMove, to);
    pointer(PrAppEventKind_PointerUp, to);
    auto selection = prUiTextEditSelection(h.ui, h.view.prompt);
    if (selection.anchor || selection.caret != 8) std::fprintf(stderr, "margin drag selected %u..%u\n", selection.anchor, selection.caret);
    CHECK(selection.anchor == 0 && selection.caret == 8);
    pointer(PrAppEventKind_PointerDown, {panel.left, panel.top - 8});
    selection = prUiTextEditSelection(h.ui, h.view.prompt);
    CHECK(selection.anchor == selection.caret);
    pointer(PrAppEventKind_PointerUp, {panel.left, panel.top - 8});
    const auto leaf = h.findText(h.view.prompt, "Wide iii");
    PrLayoutRect caret{};
    CHECK(prLayoutNodeTextCaret(h.tree, leaf, 4, PrLayoutTextAffinity_Downstream, PrLayoutCoordinates_Viewport, &caret));
    const auto r = prLayoutRectToPx(caret);
    pointer(PrAppEventKind_PointerDown, {r.left, (r.top + r.bottom) / 2});
    pointer(PrAppEventKind_PointerMove, to);
    pointer(PrAppEventKind_PointerUp, to);
    selection = prUiTextEditSelection(h.ui, h.view.prompt);
    CHECK(selection.anchor == 4 && selection.caret == 8);
}

void emptyPromptAndTranscriptShowNoText() {
    Harness h;
    h.frame();
    std::vector<std::string> texts;
    textsUnder(h, h.view.promptBox, texts);
    CHECK(texts.size() == 1);
    for (const std::string& t : texts) {
        if (t != " ")
            std::fprintf(stderr, "  text in the empty prompt: %s\n", shown(t).c_str());
        CHECK(t == " ");
    }
    CHECK(h.entries().empty());
}

bool inside(PrRect inner, PrRect outer) {
    return inner.left >= outer.left && inner.right <= outer.right && inner.top >= outer.top &&
        inner.bottom <= outer.bottom;
}

// The prompt's box at the current text scale: its corners rounded by the scaled radius, with the plus (its first child)
// and the text area inside it, the plus at its left, and the insertion segment over the edit.
void checkPromptBox(const Harness& h) {
    const float k = style::textScale(h.look.textSteps);
    PrLayoutNode box = h.view.promptBox;
    CHECK(box != kPrLayoutNodeNone);
    PrLayoutStyle record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, box));
    PrLayoutDecorationStyle d = prLayoutDecorationStyleRecord(h.tree, record.groups[PrLayoutGroup_Decoration]);
    const PrLayoutLength radius = prLayoutLengthPx(style::kPromptRadius * k);
    for (PrLayoutLength r : d.radii)
        CHECK(r.bits == radius.bits);
    PrRect outer = h.bounds(box);
    CHECK(prLayoutNodeFirstChild(h.tree, box) == h.view.prompt);
    const auto segment = prLayoutNodeNextSibling(h.tree, h.view.prompt);
    CHECK(segment != kPrLayoutNodeNone);
    if (segment != kPrLayoutNodeNone) {
        const auto cell = h.bounds(segment);
        CHECK(inside(cell, h.bounds(h.view.prompt)));
        CHECK(near(cell.right - cell.left, 0.618034f * (cell.bottom - cell.top)));
        const auto cellStyle = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, segment));
        const PrColor orange = prLayoutDecorationStyleRecord(h.tree, cellStyle.groups[PrLayoutGroup_Decoration]).background;
        CHECK(orange.a == 1 && orange.r > orange.g && orange.g > orange.b);
        CHECK(prLayoutTreeCarets(h.tree, nullptr, 0) == 0);
    }
    PrRect area = h.bounds(h.view.prompt);
    CHECK(inside(area, outer));
    CHECK(near(area.left - outer.left, style::kPromptPadX * k + 1));
    // One line: the box is kPromptHeight tall. It is kPromptPercent of the window wide, centred, and the transcript's
    // column kColumnPercent.
    CHECK(near(outer.bottom - outer.top, style::kPromptHeight * k));
    PrRect window = h.bounds(prLayoutNodeParent(h.tree, h.view.transcript));
    const float width = window.right - window.left;
    CHECK(near(outer.right - outer.left, width * style::kPromptPercent / 100));
    CHECK(near(outer.left - window.left, window.right - outer.right));
    PrRect column = h.bounds(prLayoutNodeFirstChild(h.tree, h.view.transcript));
    CHECK(near(column.right - column.left, width * h.look.prompt.width / 100));
    // The area draws no frame of its own: no fill, no border, no focus ring.
    PrLayoutStyle areaRecord = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, h.view.prompt));
    CHECK(areaRecord.groups[PrLayoutGroup_Decoration] == 0);
}

// Nothing is shown under the prompt: no model name (the owner 2026-11-10; choosing a friend's model and effort by hotkey
// is roadmap work). The footer holds the prompt's box alone, one unit above the window's bottom.
void noModelUnderThePrompt() {
    Harness h;
    h.frame();
    CHECK(!h.hasText(prLayoutTreeRoot(h.tree), "m1"));
    PrLayoutNode footer = prLayoutNodeParent(h.tree, h.view.promptBox);
    CHECK(prLayoutNodeFirstChild(h.tree, footer) == h.view.promptBox);
    CHECK(prLayoutNodeNextSibling(h.tree, h.view.promptBox) == kPrLayoutNodeNone);
    PrRect window = h.bounds(prLayoutNodeParent(h.tree, footer));
    CHECK(near(window.bottom - h.bounds(h.view.promptBox).bottom, style::kU));
    h.commandDigit(2);
    CHECK(h.active().name == "emma" && !h.hasText(prLayoutTreeRoot(h.tree), "m2"));
}

void promptIsARoundedBoxHoldingThePlus() {
    Harness h;
    h.frame();
    checkPromptBox(h);
    h.larger();
    h.frame();
    CHECK(h.look.textSteps == 1);
    checkPromptBox(h);
}

// The fill of a node's own decoration, at the last layout.
PrColor fillOf(const Harness& h, PrLayoutNode node) {
    PrLayoutStyle record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, node));
    return prLayoutDecorationStyleRecord(h.tree, record.groups[PrLayoutGroup_Decoration]).background;
}
void promptSegmentTracksInsertionAndUsesHeadroom() {
    Harness h({FriendInfo{"alex", "m1"}}, true);
    h.display.headroom = h.display.potentialHeadroom = 4;
    auto cell = [&] { return prLayoutNodeNextSibling(h.tree, h.view.prompt); };
    const auto start = h.bounds(cell());
    h.type("abc");
    h.frame();
    CHECK(h.bounds(cell()).left > start.left);
    auto redAt = [&](double phase) {
        h.time = phase - 1.0 / 60;
        h.frame();
        return fillOf(h, cell()).r;
    };
    const double rise = (1.5 - 0.2) / 1.5;
    const float floor = redAt(0);
    CHECK(floor < 1 && redAt(1.401) == floor && redAt(1.499) == floor);
    const float crest = redAt(rise);
    CHECK(crest > 1 && redAt(rise + 0.095) == crest);
    CHECK(std::abs(redAt(rise / 2) - redAt(rise + 0.1 + rise / 4)) < 0.01);
    prUiTextEditSetSelection(h.ui, h.view.prompt, {0, 2});
    h.frame();
    CHECK(cell() == kPrLayoutNodeNone);
}

void newGlyphsFadeWithoutRelightingOldText() {
    Harness h({FriendInfo{"alex", "m1"}}, true);
    h.display.headroom = h.display.potentialHeadroom = 4;
    auto settings = [&] {
        h.view.windowApp.transcriptStyle.glyphPeak = 3;
        h.view.windowApp.surface.prompt.bold = true;
        h.view.windowApp.surface.prompt.textPeak = 1.8f;
    };
    h.type("old");
    h.time += 1;
    settings();
    h.type("!");
    auto checkGlow = [&](PrLayoutNode root, const char* text, unsigned start, unsigned end) {
        auto leaf = h.findText(root, text);
        CHECK(leaf != kPrLayoutNodeNone);
        if (leaf == kPrLayoutNodeNone) return;
        PrLayoutHighlight lit{};
        const auto count = prLayoutNodeHighlights(h.tree, leaf, &lit, 1);
        if (count != 1) std::fprintf(stderr, "%s has %u glow ranges (%u..%u)\n", text, count, lit.start, lit.end);
        CHECK(count == 1);
        CHECK(lit.start == start && lit.end == end && prColorFromColor16(lit.color).r > 1);
    };
    CHECK(prLayoutNodeHighlights(h.tree, h.findText(h.view.prompt, "old!"), nullptr, 0) == 0);
    const auto value = prLayoutNodeFirstChild(h.tree, h.view.prompt);
    const auto record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, value));
    const auto font = prLayoutFontStyleRecord(h.tree, record.groups[PrLayoutGroup_Font]).font;
    CHECK(font && prTextFontStyle(font).weight == 700);
    CHECK(prColorFromColor16(prLayoutColorStyleRecord(h.tree, record.groups[PrLayoutGroup_Color]).color).r > 1);
    std::string reply = "abc";
    h.view.growing = [&](std::size_t) { return &reply; };
    settings();
    h.frame();
    h.time += 1;
    settings();
    h.frame();
    reply += "d";
    settings();
    h.frame();
    checkGlow(h.window(), "abcd", 3, 4);
    h.time += 1;
    settings();
    h.frame();
    CHECK(prLayoutNodeHighlights(h.tree, h.findText(h.window(), "abcd"), nullptr, 0) == 0);
}

PrColor borderOf(const Harness& h, PrLayoutNode node, PrRectSide side) {
    PrLayoutStyle record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, node));
    return prLayoutDecorationStyleRecord(h.tree, record.groups[PrLayoutGroup_Decoration]).borderColor[side];
}

bool colorMatches(PrColor color, unsigned rgb) {
    const auto expected = style::rgb(rgb);
    // Layout stores 16-bit floats. The tolerance is narrower than one 8-bit channel step.
    return std::abs(color.r - expected.r) < 0.001f && std::abs(color.g - expected.g) < 0.001f &&
           std::abs(color.b - expected.b) < 0.001f && color.a == 1.0f;
}

// Check the rendered text foreground and its nearest painted surface. Return the latter so a test can distinguish
// a tag inside a bubble from a tag somewhere else on the same-colored background.
PrLayoutNode checkTextPaint(const Harness& h, const std::string& text, unsigned ink, unsigned fill) {
    const auto leaf = h.findText(h.window(), text);
    CHECK(leaf != kPrLayoutNodeNone);
    if (leaf == kPrLayoutNodeNone) return leaf;
    const auto label = prLayoutNodeParent(h.tree, leaf);
    const auto record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, label));
    const auto foreground = prLayoutColorStyleRecord(h.tree, record.groups[PrLayoutGroup_Color]).color;
    if (!colorMatches(prColorFromColor16(foreground), ink)) {
        const auto color = prColorFromColor16(foreground);
        std::fprintf(stderr, "unexpected foreground for %s (%.2f,%.2f,%.2f wanted #%06x)\n",
                     text.c_str(), color.r, color.g, color.b, ink);
    }
    CHECK(colorMatches(prColorFromColor16(foreground), ink));
    for (auto n = label; n != kPrLayoutNodeNone; n = prLayoutNodeParent(h.tree, n)) {
        const auto background = fillOf(h, n);
        if (background.a > 0) {
            if (!colorMatches(background, fill))
                std::fprintf(stderr, "unexpected background for %s (%.2f,%.2f,%.2f wanted #%06x)\n",
                             text.c_str(), background.r, background.g, background.b, fill);
            CHECK(colorMatches(background, fill));
            return n;
        }
    }
    CHECK(false);
    return kPrLayoutNodeNone;
}

// In the see-through overlay only the window background and the tab strip's fill let what is behind show through:
// the prompt box, its border, the bubbles (the queued one too) and all text are opaque.
void switchNoticeBubble() {
    Harness h;
    const std::string prefix(kAccountSwitchPrefix);
    const std::string email = "subscriber@example.test";
    const std::string suffix = " (work)";
    const auto end = static_cast<std::uint32_t>(prefix.size() + email.size());
    h.model.append(0, Entry{EntryKind::Notice, prefix + email + suffix, end});
    h.frame();
    h.frame();
    const TranscriptStyle stock;
    const auto bubble = checkTextPaint(h, prefix, stock.statusInk, h.look.tabs.fill);
    CHECK(bubble == checkTextPaint(h, email, stock.switchEmailInk, h.look.tabs.fill));
    CHECK(bubble == checkTextPaint(h, suffix, stock.statusInk, h.look.tabs.fill));
    const auto prefixBounds = h.bounds(h.findText(h.window(), prefix));
    const auto emailBounds = h.bounds(h.findText(h.window(), email));
    const auto suffixBounds = h.bounds(h.findText(h.window(), suffix));
    CHECK(near(prefixBounds.top, emailBounds.top) && near(emailBounds.top, suffixBounds.top));
    CHECK(prefixBounds.right <= emailBounds.left + 1 && emailBounds.right <= suffixBounds.left + 1);
}

void copyMarkdownConversation() {
    Harness h;
    h.model.append(0, Entry{EntryKind::Person, "**Hello**"});
    h.model.append(0, Entry{EntryKind::Friend, "`world`"});
    h.frame();
    h.frame();
    const auto hello = h.findText(h.window(), "Hello");
    const auto world = h.findText(h.window(), "world");
    checkTextPaint(h, "world", TranscriptStyle{}.ink, TranscriptStyle{}.codeFill);
    CHECK(hello && world);
    if (!hello || !world) return;
    const auto span = prLayoutNodeParent(h.tree, world);
    const auto record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, span));
    const auto font = prLayoutFontStyleRecord(h.tree, record.groups[PrLayoutGroup_Font]);
    CHECK(near(prTextFontStyle(font.font).size, style::kToolSize + 2));
    auto start = h.bounds(hello), end = h.bounds(world);
    std::string copied;
    FrameHost host;
    host.view = &h.view;
    host.model = &h.model;
    h.view.copy = [&](const std::string& s) { copied = s; return true; };
    auto event = [&](PrAppEvent e) { return host.event(e); };
    CHECK(event(pointerEvent(PrAppEventKind_PointerDown, {start.left + 0.1f, (start.top + start.bottom) / 2})));
    CHECK(event(pointerEvent(PrAppEventKind_PointerMove, {end.right - 0.1f, (end.top + end.bottom) / 2})));
    CHECK(event(pointerEvent(PrAppEventKind_PointerUp, {end.right - 0.1f, (end.top + end.bottom) / 2})));
    h.frame();
    CHECK(event(keyEvent(PrAppEventKind_KeyDown, 'C', PrAppKeyCode_KeyC, style::kCommand)));
    CHECK(copied == "Hello\n\nworld");
    h.frame();
    CHECK(prLayoutNodeHighlights(h.tree, hello, nullptr, 0) == 0);
    CHECK(prLayoutNodeHighlights(h.tree, world, nullptr, 0) == 0);

    // Both endpoints are in vertical whitespace over the middle of the same word.
    const float mid = (end.left + end.right) / 2;
    const PrVec2 above{mid, end.top - 3}, below{mid, end.bottom + 3};
    CHECK(event(pointerEvent(PrAppEventKind_PointerDown, above)));
    CHECK(event(pointerEvent(PrAppEventKind_PointerMove, below)));
    CHECK(event(pointerEvent(PrAppEventKind_PointerUp, below)));
    CHECK(event(keyEvent(PrAppEventKind_KeyDown, 'C', PrAppKeyCode_KeyC, style::kCommand)));
    CHECK(copied == "world");
    CHECK(event(pointerEvent(PrAppEventKind_PointerDown, {mid, (end.top + end.bottom) / 2})));
    CHECK(event(pointerEvent(PrAppEventKind_PointerUp, below)));
    CHECK(event(keyEvent(PrAppEventKind_KeyDown, 'C', PrAppKeyCode_KeyC, style::kCommand)));
    CHECK(copied.size() < 5 && !copied.empty() && std::string("world").ends_with(copied));

    // Start in the right margin beside the final word, then drag up across both speakers.
    const auto port = h.bounds(h.view.transcript);
    const PrVec2 blank{port.right - 8, (end.top + end.bottom) / 2};
    CHECK(blank.x > end.right);
    CHECK(event(pointerEvent(PrAppEventKind_PointerDown, blank)));
    const PrVec2 first{start.left, (start.top + start.bottom) / 2};
    CHECK(event(pointerEvent(PrAppEventKind_PointerMove, first)));
    CHECK(event(pointerEvent(PrAppEventKind_PointerUp, first)));
    copied.clear();
    CHECK(event(keyEvent(PrAppEventKind_KeyDown, 'C', PrAppKeyCode_KeyC, style::kCommand)));
    CHECK(copied == "Hello\n\nworld");
}

void namedBusBubbles() {
    Harness h;
    h.model.append(0, {EntryKind::Bus, "sage\n**Build passed** and [logs](https://example.test/log)"});
    h.model.append(0, {EntryKind::Bus, "birch\nmessage.txt (4097 bytes)"});
    h.model.append(0, {EntryKind::Person, "nova-bus: this is ordinary human text"});
    for (int i = 0; i < 2; ++i) {
        h.view.windowApp.transcriptStyle.linkInk = 0x00ff00;
        h.frame();
    }
    const TranscriptStyle stock;
    CHECK(h.look.transcriptStyle.linkInk == 0x00ff00);
    const auto first = checkTextPaint(h, "sage", stock.busInk, stock.fill);
    CHECK(checkTextPaint(h, "Build passed", stock.busInk, stock.fill) == first);
    CHECK(checkTextPaint(h, "logs", stock.busInk, stock.fill) == first);
    const auto second = checkTextPaint(h, "birch", stock.busInk, stock.fill);
    CHECK(checkTextPaint(h, "message.txt (4097 bytes)", stock.busInk, stock.fill) == second);
    CHECK(first != second);
    const auto human = checkTextPaint(h, "nova-bus: this is ordinary human text", stock.personInk, stock.fill);
    CHECK(h.bounds(first).left < h.bounds(human).left);
    CHECK(h.bounds(h.findText(h.window(), "sage")).bottom <= h.bounds(h.findText(h.window(), "Build passed")).top + 1);
}

void markdownListsAndLinks() {
    Harness h;
    const std::string words = "First line wraps onto the next when several more words fill the available width and force it to wrap across the column.";
    h.model.append(0, {EntryKind::Friend, "9. " + words + "\n10. Second  \n    continuation\n    - Nested [web link](https://example.org/path) "});
    h.frame();
    h.frame();
    auto first = h.findText(h.window(), words);
    auto second = h.findText(h.window(), "Second\ncontinuation");
    auto nested = h.findText(h.window(), "web link");
    CHECK(first && second && nested);
    if (!first || !second || !nested) return;
    PrLayoutRect lines[16]{};
    auto boxes = [&](PrLayoutNode node, size_t size) {
        return prLayoutNodeTextRangeBoxes(h.tree, node, 0, size, PrLayoutCoordinates_Viewport, lines, 16);
    };
    CHECK(boxes(first, words.size()) >= 2);
    float x = prLayoutUnitToPx(lines[0].left);
    CHECK(near(x, prLayoutUnitToPx(lines[1].left)));
    CHECK(boxes(second, 19) >= 2);
    CHECK(near(x, prLayoutUnitToPx(lines[0].left)) && near(x, prLayoutUnitToPx(lines[1].left)));
    CHECK(h.bounds(nested).left > x);
    auto link = h.bounds(nested);
    const PrVec2 at{(link.left + link.right) / 2, (link.top + link.bottom) / 2};
    FrameHost host;
    host.model = &h.model;
    host.view = &h.view;
    std::vector<std::string> opened;
    h.view.openLink = [&](const std::string& url) { opened.push_back(url); };
    auto pointer = [&](PrAppEventKind kind, PrVec2 where) { return host.event(pointerEvent(kind, where)); };
    CHECK(pointer(PrAppEventKind_PointerDown, at));
    h.frame();
    CHECK(pointer(PrAppEventKind_PointerUp, at));
    CHECK(opened == std::vector<std::string>{"https://example.org/path"});
    CHECK(pointer(PrAppEventKind_PointerDown, at));
    CHECK(pointer(PrAppEventKind_PointerMove, {at.x - 12, at.y}));
    CHECK(pointer(PrAppEventKind_PointerUp, at));
    CHECK(opened.size() == 1);
    checkTextPaint(h, "web link", h.look.tabs.selectedFill, h.look.tabs.fill);
}

void longInlineCommandWrapsInTheReply() {
    const std::string lead = "The hook works.";
    const std::string prose = " I started it under Monitor with the full stream, using the new build's wrapper:";
    const std::string command = "/home/friend/.nexus/server/builds/" + std::string(64, 'a') +
        "/drivers/claude/bin/bus/nova-bus hook --as friend --harness claude";
    const auto intro = "**" + lead + "**" + prose;
    struct Layout { float width; int zoom; float replyWidth; };
    for (auto layout : {Layout{2525, 0, TranscriptStyle{}.replyWidth}, Layout{1450, 0, 72}, Layout{1080, 1, 56}}) {
        Harness h;
        h.extent = h.view.extent = {layout.width, 1400};
        h.view.askedTextSteps = layout.zoom;
        h.model.append(0, {EntryKind::Friend, intro});
        // A single LF before the backtick is a Markdown soft break. The paragraph's last word still fits
        // exactly where it would have appeared without the following inline command.
        h.model.append(0, {EntryKind::Friend, intro + "\n`" + command + "`"});
        for (int i = 0; i < 2; ++i) {
            h.view.windowApp.surface.prompt.width = 98;
            h.view.windowApp.transcriptStyle.replyWidth = layout.replyWidth;
            h.frame();
        }
        const auto entries = h.entries();
        CHECK(entries.size() == 2);
        if (entries.size() != 2) continue;
        const auto wrapperPosition = [&](PrLayoutNode entry, const std::string& text) {
            const auto body = h.findText(entry, text);
            const auto heading = h.findText(entry, lead);
            PrLayoutRect word{};
            CHECK(body && heading);
            if (!body || !heading) return PrVec2{};
            CHECK(prLayoutNodeTextRangeBoxes(h.tree, body, prose.find("wrapper:"), prose.size(),
                PrLayoutCoordinates_Viewport, &word, 1) == 1);
            return PrVec2{prLayoutUnitToPx(word.left) - h.bounds(heading).left,
                prLayoutUnitToPx(word.top) - h.bounds(heading).top};
        };
        const auto reference = wrapperPosition(entries[0], prose);
        const auto mixed = wrapperPosition(entries[1], prose + " ");
        CHECK(near(reference.x, mixed.x) && near(reference.y, mixed.y));
        const auto code = h.findText(entries[1], command);
        CHECK(code);
        if (!code) continue;
        PrLayoutRect lines[64]{};
        const auto count = prLayoutNodeTextRangeBoxes(h.tree, code, 0, command.size(),
            PrLayoutCoordinates_Viewport, lines, 64);
        CHECK(count > 1 && count < 64);
        const auto reply = h.bounds(prLayoutNodeFirstChild(h.tree, entries[1]));
        for (unsigned i = 0; i < count && i < 64; ++i) {
            const auto line = prLayoutRectToPx(lines[i]);
            CHECK(line.left >= reply.left - 1 && line.right <= reply.right + 1);
        }
    }
}

void conversationBubblesAndSignInCard() {
    Harness h;
    const std::string live = "Streaming reply";
    h.view.growing = [&](std::size_t) { return &live; };
    h.model.append(0, Entry{EntryKind::Person, "Hello"});
    h.model.append(0, Entry{EntryKind::Friend, "A reply."});
    h.model.append(0, Entry{EntryKind::Notice, "Usage limit reached."});
    h.frame();
    h.frame();
    const auto entries = h.entries();
    CHECK(entries.size() == 4);
    if (entries.size() != 4) return;
    const auto bubble = [&](std::size_t i) { return prLayoutNodeFirstChild(h.tree, entries[i]); };
    CHECK(colorMatches(fillOf(h, bubble(0)), TranscriptStyle{}.fill));
    CHECK(colorMatches(fillOf(h, h.view.conversation), h.look.tabs.fill));
    CHECK(fillOf(h, bubble(1)).a == 0 && fillOf(h, bubble(3)).a == 0);
    CHECK(h.bounds(bubble(0)).left > h.bounds(bubble(1)).left);
    CHECK(near(h.bounds(bubble(1)).left, h.bounds(bubble(3)).left));
    // A stored, closed Notice uses the same model entry after hydration. Live status and chat keep distinct colors.
    const TranscriptStyle stock;
    checkTextPaint(h, "Hello", stock.ink, stock.fill);
    checkTextPaint(h, "A reply.", stock.ink, h.look.tabs.fill);
    checkTextPaint(h, "Usage limit reached.", stock.noticeInk, h.look.tabs.fill);
    checkTextPaint(h, live, stock.ink, h.look.tabs.fill);
    CHECK(stock.noticeInk == 0xffb4b4 && stock.ink != stock.noticeInk && stock.statusInk != stock.noticeInk);

    const std::string email = "a.long.selected.account.for.the.narrow.overlay@example.test";
    h.view.signInAccount = [&] { return SignInAccount{"work", "Work", email}; };
    int activations = 0;
    h.view.activateSignIn = [&] { ++activations; return true; };
    h.frame();
    h.frame();
    CHECK(!h.hasText(h.window(), "A reply."));
    CHECK(h.view.prompt != kPrLayoutNodeNone);
    auto area = prLayoutNodeFirstChild(h.tree, h.window());
    while (area != kPrLayoutNodeNone && !h.hasText(area, "Sign in to Work"))
        area = prLayoutNodeNextSibling(h.tree, area);
    CHECK(area != kPrLayoutNodeNone);
    if (area == kPrLayoutNodeNone) return;
    const auto button = prLayoutNodeFirstChild(h.tree, area);
    const auto fill = fillOf(h, button);
    CHECK(colorMatches(fill, h.look.messageCard.fill));
    const std::string hint = "Press Enter or click to sign in.";
    CHECK(h.hasText(button, hint));
    const auto title = prLayoutNodeFirstChild(h.tree, button);
    const auto address = prLayoutNodeNextSibling(h.tree, title);
    const auto instruction = prLayoutNodeNextSibling(h.tree, address);
    CHECK(h.hasText(title, "Sign in to Work") && h.hasText(address, email) && h.hasText(instruction, hint));
    CHECK(prLayoutNodeNextSibling(h.tree, instruction) == kPrLayoutNodeNone);
    checkTextPaint(h, "Sign in to Work", h.look.messageCard.ink, h.look.messageCard.fill);
    checkTextPaint(h, email, h.look.messageCard.emailInk, h.look.messageCard.fill);
    checkTextPaint(h, hint, h.look.messageCard.ink, h.look.messageCard.fill);
    CHECK(near(h.centre(button).x, h.centre(area).x));
    CHECK(near(h.centre(button).y, h.centre(area).y));
    CHECK(h.height(area) > 2 * h.height(button));
    h.pointer(PrAppEventKind_PointerDown, h.centre(button));
    h.pointer(PrAppEventKind_PointerUp, h.centre(button));
    CHECK(activations == 1);
    h.extent = h.view.extent = {440, 780};
    h.frame();
    h.frame();
    const auto narrowAddress = prLayoutNodeParent(h.tree, h.findText(h.window(), email));
    const auto narrowButton = prLayoutNodeParent(h.tree, narrowAddress);
    const auto narrowArea = prLayoutNodeParent(h.tree, narrowButton);
    if (!(h.height(narrowAddress) > 2 * h.look.messageCard.textSize)) {
        const auto addr = h.bounds(narrowAddress), box = h.bounds(narrowButton), areaBox = h.bounds(narrowArea);
        std::fprintf(stderr, "narrow email geometry: address %.0f x %.0f, card %.0f x %.0f, area %.0f x %.0f\n",
            addr.right - addr.left, addr.bottom - addr.top, box.right - box.left, box.bottom - box.top,
            areaBox.right - areaBox.left, areaBox.bottom - areaBox.top);
    }
    CHECK(h.height(narrowAddress) > 2 * h.look.messageCard.textSize);
    CHECK(h.bounds(narrowAddress).left >= h.bounds(narrowArea).left &&
          h.bounds(narrowAddress).right <= h.bounds(narrowArea).right);
    h.view.signInAccount = [] { return SignInAccount{"legacy", "", ""}; };
    h.frame();
    h.frame();
    const auto legacy = prLayoutNodeParent(h.tree, h.findText(h.window(), "Sign in to legacy"));
    const auto legacyHint = prLayoutNodeNextSibling(h.tree, legacy);
    CHECK(h.hasText(legacyHint, hint) && prLayoutNodeNextSibling(h.tree, legacyHint) == kPrLayoutNodeNone);
    h.view.signInAccount = {};
    h.frame();
    CHECK(h.hasText(h.window(), "A reply."));
    CHECK(!h.hasText(h.window(), "Sign in to Work"));
}

void overlayKeepsAllButTheBackgroundOpaque() {
    Harness h(std::vector<FriendInfo>{FriendInfo{"alex", "m1"}, FriendInfo{"emma", "m2"}}, true);
    h.type("sent");
    h.enter();
    h.type("held");
    h.model.hold_draft(h.model.active());
    h.time = 1008; // crest of the 2.8-second unsent-text pulse
    h.frame();
    PrLayoutNode window = prLayoutNodeParent(h.tree, h.view.transcript);
    CHECK(near(fillOf(h, window).a * 100, 60.0f));
    CHECK(prColorFromColor16(prLayoutTreeCanvasColor(h.tree)).a == 0);
    CHECK(fillOf(h, prLayoutNodeParent(h.tree, window)).a == 0);
    CHECK(near(h.kit->theme.palette.bg0.a * 100, style::kOverlayOpacity * 100));
    CHECK(near(h.kit->theme.palette.bg1.a * 100, style::kOverlayStripFill * 100));
    CHECK(fillOf(h, h.view.promptBox).a == 1);
    for (int side = 0; side < PrRectSide_Count; ++side)
        CHECK(borderOf(h, h.view.promptBox, PrRectSide(side)).a == 1);
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 2);
    for (PrLayoutNode entry : e) {
        PrLayoutNode bubble = prLayoutNodeFirstChild(h.tree, entry);
        CHECK(fillOf(h, bubble).a == 1);
    }
    const TranscriptStyle stock;
    const auto queued = checkTextPaint(h, "held", stock.ink, stock.fill);
    CHECK(!h.hasText(queued, "queued"));
    const auto border = prLayoutNodeLastChild(h.tree, queued);
    const auto before = prLayoutNodeContent(h.tree, border);
    CHECK(before.kind == PrLayoutContentKind_Painter);
    h.time += 0.25;
    h.frame();
    CHECK(prLayoutNodeContent(h.tree, border).revision != before.revision);
    for (auto tab : h.tabs())
        CHECK(fillOf(h, prLayoutNodeParent(h.tree, tab)).a == 1);
    h.time = 1201;
    h.frame();
    const float selectedAlpha = fillOf(h, h.view.tabNodes[h.model.active()]).a;
    CHECK(selectedAlpha > 0 && selectedAlpha < 1);
    h.time += 2;
    h.frame();
    CHECK(std::abs(fillOf(h, h.view.tabNodes[h.model.active()]).a - selectedAlpha) > 0.01f);
    const PrKitPalette& p = h.kit->theme.palette;
    for (PrColor c : {p.text1, p.text2, p.text3, p.text4, p.textSelection})
        CHECK(c.a == 1);
    for (PrKitLookId id : std::initializer_list<PrKitLookId>{PrKitLookId_Tab, PrKitLookId_TabChosen})
        CHECK(h.kit->looks[id].text.a == 1);
}

// The widest right edge, and the leftmost left edge, of `node` and everything under it, at the last layout.
void extent(const Harness& h, PrLayoutNode node, float* left, float* right) {
    PrRect b = h.bounds(node);
    *left = std::min(*left, b.left);
    *right = std::max(*right, b.right);
    for (PrLayoutNode c = prLayoutNodeFirstChild(h.tree, node); c != kPrLayoutNodeNone;
            c = prLayoutNodeNextSibling(h.tree, c))
        extent(h, c, left, right);
}

bool within(const Harness& h, PrLayoutNode node, PrRect outer) {
    float left = 1e9f;
    float right = -1e9f;
    extent(h, node, &left, &right);
    if (left < outer.left - 0.5f || right > outer.right + 0.5f) {
        std::fprintf(stderr, "  laid out %.1f..%.1f, room %.1f..%.1f\n", double(left), double(right),
            double(outer.left), double(outer.right));
        return false;
    }
    return true;
}

// No friends: no tabs and no prompt; the transcript shows the notice (why there are none) and nothing else.
void noFriendsShowsTheNotice() {
    Harness h(std::vector<FriendInfo>{});
    h.view.notice = "No friends to show.\nnova-config listed no enabled friends";
    h.frame();
    h.frame();
    CHECK(h.tabs().empty());
    CHECK(h.view.prompt == kPrLayoutNodeNone);
    std::vector<std::string> texts;
    textsUnder(h, h.view.transcript, texts);
    CHECK(texts.size() == 1 && texts[0] == h.view.notice);
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 1);
    PrRect port = h.bounds(h.view.transcript);
    CHECK(e.size() == 1 && within(h, e[0], port));
    CHECK(e.size() == 1 && h.height(e[0]) > style::kLinePitch);    // two lines
    // Keys change nothing and nothing fails: a digit, Enter, a text key.
    h.commandDigit(1);
    h.enter();
    h.type("x");
    CHECK(h.model.tab_count() == 0);
}

// A binding creates the primary chat on a window whose server has no configured friends. Exercise
// the same keyboard composer and streaming transcript as a named conversation.
void primaryChatUsesTheOrdinaryComposer() {
    Harness h(std::vector<FriendInfo>{});
    test::FakeLink link;
    FrameHost host;
    host.model = &h.model;
    host.view = &h.view;
    host.connect(&link, "test");
    link.friends({});
    host.pollLink();
    h.frame();
    CHECK(h.tabs().empty());
    CHECK(h.view.prompt == kPrLayoutNodeNone);
    CHECK(link.sent.empty());

    const auto publish = [&](std::vector<sync::EntryRef> entries) {
        auto session = std::make_shared<sync::SessionView>();
        session->id = 41;
        session->info.friendName = kPrimaryChat;
        session->info.harness = "codex";
        session->entries = std::move(entries);
        sync::Sessions state({session});
        state.setFriends({});
        host.showState(state, false);
        h.frame();
        h.frame();
    };
    publish({});
    CHECK(h.labels() == std::vector<std::string>({"Chat"}));
    if (h.model.tab_count() != 1) return;
    CHECK(h.view.prompt != kPrLayoutNodeNone && prUiFocus(h.ui) == h.view.prompt);
    h.view.windowApp.surface.prompt.showPlus = true;
    h.frame();
    CHECK(prLayoutNodeFirstChild(h.tree, h.view.promptBox) == h.view.prompt);
    CHECK(link.sent.empty());
    h.type("Where shall we start?");
    h.enter();
    CHECK(link.sent.size() == 1);
    if (link.sent.size() != 1) return;
    CHECK(link.sent.front().kind == sync::ActionKind::Send && link.sent.front().session == 41 &&
        link.sent.front().text == "Where shall we start?");

    auto question = std::make_shared<const sync::SessionEntry>(
        sync::SessionEntry{EntryKind::Person, "Where shall we start?", true});
    publish({question, std::make_shared<const sync::SessionEntry>(sync::SessionEntry{EntryKind::Friend, "Right", false})});
    CHECK(h.hasText(h.window(), "Where shall we start?") && h.hasText(h.window(), "Right"));
    CHECK(h.active().transcript.size() == 1);
    publish({question, std::make_shared<const sync::SessionEntry>(sync::SessionEntry{EntryKind::Friend, "Right here.", true})});
    CHECK(h.active().transcript.size() == 2 && h.hasText(h.window(), "Right here."));
    CHECK(!h.hasText(h.window(), "Right") && link.sent.size() == 1);
}

// A long URL or token with no break opportunity wraps inside the column, the bubble and the prompt.
void longUnbrokenTextWraps() {
    Harness h;
    const std::string token = "https://example.com/" + std::string(580, 'x');
    h.model.append(0, Entry{EntryKind::Friend, token});
    h.model.append(0, Entry{EntryKind::Person, token});
    h.model.set_draft(0, token);
    h.frame();
    h.frame();
    PrRect column = h.bounds(prLayoutNodeFirstChild(h.tree, h.view.transcript));
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 2);
    for (PrLayoutNode entry : e)
        CHECK(within(h, entry, column));
    CHECK(within(h, h.view.prompt, h.bounds(h.view.promptBox)));
    // It wrapped rather than shrank: each is several lines tall.
    CHECK(e.size() == 2 && h.height(e[0]) > 3 * style::kLinePitch);
    CHECK(e.size() == 2 && h.height(e[1]) > 3 * style::kLinePitch);
}

// Each tool call is one compact line: tool lines in a row stand together in one item of the column, each one line
// high however long its summary (cut at the column's edge, never wrapped), and close together.
void toolLinesAreOneCompactLine() {
    Harness h;
    h.model.append(0, Entry{EntryKind::Friend, "Let me look."});
    h.model.append(0, Entry{EntryKind::Tool, "Bash: ls src"});
    h.model.append(0, Entry{EntryKind::Tool, "Read: " + std::string(400, 'x')});
    h.model.append(0, Entry{EntryKind::Friend, "Done."});
    h.frame();
    h.frame();
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 3);
    if (e.size() != 3) {
        return;
    }
    const TranscriptStyle stock;
    checkTextPaint(h, "Bash: ls src", stock.statusInk, stock.fill);
    checkTextPaint(h, std::string(400, 'x'), h.look.tabs.selectedFill, stock.fill);
    checkTextPaint(h, "Let me look.", stock.ink, h.look.tabs.fill);
    std::vector<PrLayoutNode> lines;
    for (PrLayoutNode n = prLayoutNodeFirstChild(h.tree, e[1]); n != kPrLayoutNodeNone;
            n = prLayoutNodeNextSibling(h.tree, n)) {
        lines.push_back(n);
    }
    CHECK(lines.size() == 2);
    PrRect column = h.bounds(prLayoutNodeFirstChild(h.tree, h.view.transcript));
    for (PrLayoutNode line : lines) {
        CHECK(near(h.height(line), style::kToolPitch));
        CHECK(h.bounds(line).right <= column.right + 0.5f);
    }
    if (lines.size() == 2) {
        CHECK(near(h.bounds(lines[1]).top - h.bounds(lines[0]).bottom, style::kToolGap));
    }
}

// The friend's text still growing shows after the transcript, as it grows, and goes when it closes (the transcript
// then holds it); the queue stays after it.
void growingTextShowsAfterTheTranscript() {
    Harness h;
    std::string growing;
    h.view.growing = [&](std::size_t id) -> const std::string* {
        return id == h.model.tab(0).id && !growing.empty() ? &growing : nullptr;
    };
    h.model.append(0, Entry{EntryKind::Person, "hi"});
    h.frame();
    CHECK(h.entries().size() == 1);
    growing = "Hel";
    h.frame();
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 2 && h.hasText(e.back(), "Hel"));
    growing = "Hello, there";
    h.frame();
    e = h.entries();
    CHECK(e.size() == 2 && h.hasText(e.back(), "Hello, there"));
    h.model.set_draft(0, "later");
    h.model.queue(0);
    h.frame();
    e = h.entries();
    CHECK(e.size() == 3 && h.hasText(e[1], "Hello, there") && h.hasText(e[2], "later"));
    growing.clear();
    h.model.append(0, Entry{EntryKind::Friend, "Hello, there."});
    h.frame();
    e = h.entries();
    CHECK(e.size() == 3 && h.hasText(e[1], "Hello, there.") && !h.hasText(e[1], "Hello, there"));
    h.commandDigit(2);
    CHECK(h.entries().empty());     // emma's tab: alex's growing text is alex's
}

// The state cursor: on its own line just under the last text, before the queue; the busy one breathes (its fill
// changes with the frame's time, and the window asks for frames while it shows); the others are steady; none without
// the hook or for None.
void stateCursorUnderTheLastText() {
    Harness h;
    h.model.append(0, Entry{EntryKind::Friend, "Hello."});
    CursorState state = CursorState::None;
    h.view.cursor = [&](std::size_t) { return state; };
    h.frame();
    CHECK(h.entries().size() == 1 && !h.view.breathing && h.view.energyAnimating && windowWantsFrame(h.view));
    h.model.set_draft(0, "later");
    h.model.queue(0);
    unsigned agents = 1;
    h.view.activeAgents = [&](std::size_t) { return agents; };
    auto lamps = [&] {
        std::vector<PrLayoutNode> cells;
        for (auto n = prLayoutNodeFirstChild(h.tree, h.entries().at(1)); n != kPrLayoutNodeNone;
             n = prLayoutNodeNextSibling(h.tree, n)) cells.push_back(n);
        return cells;
    };
    state = CursorState::Busy;
    h.frame();
    CHECK(lamps().size() == 1 && h.view.breathing);
    const auto trough = fillOf(h, lamps().front());
    h.time += h.look.transcriptStyle.scannerSeconds / 2;
    h.frame();
    CHECK(fillOf(h, lamps().front()).b > trough.b + 0.2f);
    agents = 4;
    state = CursorState::Working;
    h.frame();
    CHECK(lamps().size() == agents);
    auto blueAt = [&](double phase, unsigned cell) {
        h.time = h.view.scannerStarted + h.look.transcriptStyle.scannerSeconds * phase - 1.0 / 60;
        h.frame();
        return fillOf(h, lamps()[cell]).b;
    };
    const auto rightPeak = blueAt(0.52, 3);
    CHECK(blueAt(0.56, 3) == rightPeak);
    CHECK(blueAt(0.57, 0) < rightPeak * 0.5f);
    const auto returning = blueAt(0.79, 1);
    CHECK(returning > blueAt(0.80, 3));
    CHECK(near(blueAt(1.02, 0), rightPeak));
    CHECK(near(blueAt(1.06, 0), rightPeak));
    for (int zoom = 0; zoom < 2; ++zoom) {
        if (zoom) { h.larger(); h.frame(); }
        const auto caret = h.bounds(prLayoutNodeNextSibling(h.tree, h.view.prompt));
        CHECK(near(h.bounds(lamps().front()).left, caret.left));
        CHECK(near(h.bounds(h.findText(h.window(), "Hello.")).left, caret.left));
        for (auto cell : lamps()) {
            const auto segment = h.bounds(cell);
            CHECK(near(segment.right - segment.left, caret.right - caret.left));
            CHECK(near(segment.bottom - segment.top, caret.bottom - caret.top));
            const auto color = fillOf(h, cell);
            CHECK(color.b > color.r);
        }
    }
    h.smaller();
    h.frame();
    state = CursorState::Idle;
    h.frame();
    CHECK(lamps().size() == agents);
    for (int i = 0; i < 32; ++i) h.frame();
    CHECK(lamps().size() == 1 && !h.view.breathing);
    CHECK(colorMatches(fillOf(h, lamps()[0]), h.look.prompt.fill));
    for (auto status : {CursorState::Down, CursorState::Lost}) {
        state = status;
        h.frame();
        CHECK(lamps().empty());
        CHECK(near(h.height(h.entries()[1]), style::kCursorHeight));
        CHECK(h.hasText(h.entries()[2], "later"));
    }
    // The text scale scales it.
    state = CursorState::Idle;
    h.larger();
    h.frame();
    h.frame();
    CHECK(h.entries().size() == 3 && near(h.height(h.entries()[1]), style::kCursorHeight * style::textScale(1)));
    // Without the hook there is none, and nothing breathes.
    state = CursorState::Busy;
    h.frame();
    CHECK(h.view.breathing);
    h.view.cursor = nullptr;
    h.frame();
    CHECK(h.entries().size() == 2 && !h.view.breathing);
}

void nativeBusSendIsPrivateInHistory() {
    Harness h;
    h.model.append(0, Entry{EntryKind::Tool, "Read: before"});
    h.model.append(0, Entry{EntryKind::Tool, "mcp__nexus__nexus_bus_send: private body"});
    h.model.append(0, Entry{EntryKind::Tool, "SendMessage: private child message"});
    h.model.append(0, Entry{EntryKind::Tool, "Agent: child launch description"});
    h.model.append(0, Entry{EntryKind::Tool, "Read: after"});
    h.frame();
    CHECK(h.entries().size() == 2);
    CHECK(!h.hasText(h.window(), "Agent: child launch description"));
    CHECK(!h.hasText(h.window(), "SendMessage: private child message"));
    CHECK(!h.hasText(h.window(), "mcp__nexus__nexus_bus_send: private body"));
}

void scannerHasPromptPadding() {
    Harness h;
    h.model.append(0, Entry{EntryKind::Friend, "Finished."});
    h.view.cursor = [](std::size_t) { return CursorState::Busy; };
    h.look.prompt.caretOffsetY = 2;
    for (int zoom = 0; zoom < 2; ++zoom) {
        if (zoom) h.larger();
        h.frame();
        h.frame();
        const auto lamp = h.bounds(prLayoutNodeFirstChild(h.tree, h.entries().back()));
        const auto text = h.bounds(h.entries().front());
        const auto panel = h.bounds(h.view.conversation);
        const auto caret = h.bounds(prLayoutNodeNextSibling(h.tree, h.view.prompt));
        const auto prompt = h.bounds(h.view.promptBox);
        CHECK(std::abs((lamp.top - text.bottom) - (caret.top - prompt.top)) < 1);
        CHECK(std::abs((panel.bottom - lamp.bottom) - (prompt.bottom - caret.bottom)) < 1);
    }
}

void sendingKeepsBubbleOnScannerRow() {
    Harness h;
    h.view.cursor = [](std::size_t) { return CursorState::Busy; };
    h.model.set_draft(0, "Same place");
    h.model.hold_draft(0);
    h.frame();
    h.frame();
    const auto held = h.bounds(h.findText(h.window(), "Same place"));
    const auto scanner = h.bounds(prLayoutNodeFirstChild(h.tree, h.entries().front()));
    const auto heldBubble = h.bounds(prLayoutNodeFirstChild(h.tree, h.entries().back()));
    const auto panel = h.bounds(h.view.conversation);
    CHECK(near(panel.right - heldBubble.right, panel.bottom - heldBubble.bottom));
    CHECK(near(heldBubble.bottom, scanner.bottom));
    CHECK(scanner.top < held.bottom + style::kBubblePadY && scanner.bottom > held.top);
    h.model.pop_held(0);
    h.model.append(0, Entry{EntryKind::Person, "Same place"});
    h.frame();
    h.frame();
    const auto sent = h.bounds(h.findText(h.window(), "Same place"));
    const auto sentBubble = h.bounds(prLayoutNodeFirstChild(h.tree, h.entries().front()));
    CHECK(near(panel.right - sentBubble.right, panel.bottom - sentBubble.bottom));
    CHECK(near(sentBubble.bottom, heldBubble.bottom));
    CHECK(near(held.top, sent.top) && near(held.left, sent.left));
}

// Nothing stands under or around an open prompt but its box: the link's line shows only in the prompt's place before
// it opens, and why a prompt was not sent shows at the end of the transcript.
void nothingAroundTheOpenPrompt() {
    Harness h;
    h.view.status = "No contact with studio for 7 s";
    h.view.problem = "This prompt is 40 KiB; the limit is 32 KiB.";
    h.frame();
    h.frame();
    PrLayoutNode footer = prLayoutNodeParent(h.tree, h.view.promptBox);
    CHECK(prLayoutNodeFirstChild(h.tree, footer) == h.view.promptBox);
    CHECK(prLayoutNodeNextSibling(h.tree, h.view.promptBox) == kPrLayoutNodeNone);
    CHECK(!h.hasText(prLayoutTreeRoot(h.tree), h.view.status));
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(!e.empty() && h.hasText(e.back(), h.view.problem));
    h.view.problem.clear();
    h.view.promptOpen = false;
    h.view.status = "Connecting to studio";
    h.frame();
    CHECK(h.view.promptBox == kPrLayoutNodeNone && h.hasText(prLayoutTreeRoot(h.tree), "Connecting to studio"));
    const TranscriptStyle stock;
    checkTextPaint(h, "Connecting to studio", stock.statusInk, stock.fill);
    CHECK(h.entries().empty());
}

// Text cut to the prompt's room ends on a whole UTF-8 character, and the room is what a friend's draft leaves.
void promptCutKeepsWholeCharacters() {
    const std::string text = "ab\xC3\xA9" "c";      // a, b, e-acute (two bytes), c
    const PrStringView all{text.data(), text.size()};
    CHECK(cutText(all, 10).size == 5);
    CHECK(cutText(all, 4).size == 4);
    CHECK(cutText(all, 3).size == 2);               // not half of the e-acute
    CHECK(cutText(all, 0).size == 0);
    Harness h;
    CHECK(promptRoom(h.view, h.ui) == kPromptBytes - h.model.tab(h.model.active()).draft.size());
    h.model.set_draft(h.model.active(), std::string(kPromptBytes + 10, 'x'));
    CHECK(promptRoom(h.view, h.ui) == 0);
    CHECK(cutLine(1048576, 32768) == "1,015,808 of 1,048,576 bytes were left out: the prompt holds 32 KB.");
    CHECK(cutLine(1, 0) == "1 of 1 bytes were left out: the prompt holds 32 KB.");
}

// The room the prompt has counts its selection, which what comes in replaces: a full prompt all selected has room for
// a whole prompt, and typing over it replaces it.
void promptRoomCountsTheSelection() {
    Harness h;
    h.model.set_draft(h.model.active(), std::string(kPromptBytes, 'x'));
    h.frame();
    CHECK(promptRoom(h.view, h.ui) == 0);
    prUiTextEditSetSelection(h.ui, h.view.prompt, PrUiTextSelection{0, uint32_t(kPromptBytes)});
    h.frame();
    CHECK(promptRoom(h.view, h.ui) == kPromptBytes);
    prUiTextEditSetSelection(h.ui, h.view.prompt, PrUiTextSelection{uint32_t(kPromptBytes), 100});
    h.frame();
    CHECK(promptRoom(h.view, h.ui) == kPromptBytes - 100);
    h.type("y");
    CHECK(h.active().draft == std::string(100, 'x') + "y");
}

// The line about text cut to fit the prompt: at the end of the transcript of the tab it was cut on, with nothing
// around the prompt, on that tab only, until its draft changes. Text still on its way into the prompt when the line was
// made is not a change.
void cutLineStaysWithItsTabAndDraft() {
    Harness h;
    for (int i = 0; i < 400; ++i) {
        h.model.append(0, Entry{EntryKind::Friend, "line " + std::to_string(i)});
    }
    h.model.set_draft(0, "abc");
    h.frame();
    h.frame();
    // Scrolled up: the line brings the transcript to its end, where it shows.
    PrRect port = h.bounds(h.view.transcript);
    PrVec2 middle{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
    h.push(pointerEvent(PrAppEventKind_PointerMove, middle));
    h.push(scrollEvent(middle, PrVec2{0, -3000}));
    h.frame();
    promptCut(h.view, 10, 3, false);
    h.frame();
    h.frame();
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(!e.empty() && h.hasText(e.back(), cutLine(10, 3)));
    if (!e.empty() && h.bounds(e.back()).bottom > port.bottom + 0.5f) {
        std::fprintf(stderr, "  cut line at %.1f..%.1f, out of the view %.1f..%.1f\n", double(h.bounds(e.back()).top),
            double(h.bounds(e.back()).bottom), double(port.top), double(port.bottom));
    }
    CHECK(!e.empty() && h.bounds(e.back()).bottom <= port.bottom + 0.5f && h.bounds(e.back()).top >= port.top);
    PrLayoutNode footer = prLayoutNodeParent(h.tree, h.view.promptBox);
    CHECK(prLayoutNodeFirstChild(h.tree, footer) == h.view.promptBox);
    CHECK(prLayoutNodeNextSibling(h.tree, h.view.promptBox) == kPrLayoutNodeNone);
    // emma's tab shows none; back on alex's it shows.
    h.commandDigit(2);
    h.frame();
    CHECK(!h.hasText(prLayoutTreeRoot(h.tree), cutLine(10, 3)));
    h.commandDigit(1);
    h.frame();
    CHECK(h.hasText(prLayoutTreeRoot(h.tree), cutLine(10, 3)));
    // The draft changes: the line goes.
    h.type("d");
    CHECK(h.view.cut.empty() && !h.hasText(prLayoutTreeRoot(h.tree), cutLine(10, 3)));
    // Cut text on its way in: its arrival keeps the line, the next edit ends it.
    promptCut(h.view, 20, 3, true);
    const std::string kept = "xyz";
    h.push(textEvent(PrAppEventKind_Paste, PrStringView{kept.data(), kept.size()}));
    h.frame();
    CHECK(h.active().draft == "abcdxyz");
    CHECK(h.view.cut == cutLine(20, 3) && !h.view.cutComing);
    h.frame();
    CHECK(h.hasText(prLayoutTreeRoot(h.tree), cutLine(20, 3)));
    h.key(PrAppKey_Backspace, PrAppKeyCode_Backspace);
    CHECK(h.view.cut.empty());
    // A send changes the draft too.
    promptCut(h.view, 20, 3, false);
    h.enter();
    h.frame();
    CHECK(h.view.cut.empty());
}

// A long transcript declares only the entries in sight and a screen either side (view.h TranscriptItems), with tool
// runs kept together; scrolled to the top, the oldest entry shows, and back at the bottom the newest.
void longTranscriptDeclaresWhatIsInSight() {
    Harness h;
    for (int i = 0; i < 2000; ++i) {
        const EntryKind kind = i % 3 == 0 ? EntryKind::Person : i % 3 == 1 ? EntryKind::Friend : EntryKind::Tool;
        h.model.append(0, Entry{kind, "entry " + std::to_string(i)});
    }
    h.frame();
    h.frame();
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() < 80);
    CHECK(!e.empty() && h.hasText(e.back(), "entry 1999"));
    PrRect port = h.bounds(h.view.transcript);
    PrVec2 middle{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
    h.push(pointerEvent(PrAppEventKind_PointerMove, middle));
    for (int i = 0; i < 40; ++i) {
        h.push(scrollEvent(middle, PrVec2{0, -1000000}));
        h.frame();
    }
    e = h.entries();
    CHECK(e.size() < 80);
    bool oldest = false;
    for (PrLayoutNode n : e)
        oldest = oldest || h.hasText(n, "entry 0");
    CHECK(oldest);
    for (int i = 0; i < 40; ++i) {
        h.push(scrollEvent(middle, PrVec2{0, 1000000}));
        h.frame();
    }
    e = h.entries();
    CHECK(!e.empty() && h.hasText(e.back(), "entry 1999"));
    CHECK(!e.empty() && h.bounds(e.back()).bottom <= port.bottom + 0.5f);
}

// A tab shown again with a scroll past the end of its content (kept from a taller layout) declares its last item, and
// the spacer above it stands for the items before it only: the column is as tall as with every item declared.
void scrollPastTheEndCountsTheLastItemOnce() {
    Harness h;
    for (int i = 0; i < 8; ++i) {
        h.model.append(0, Entry{EntryKind::Friend, "line " + std::to_string(i)});
    }
    h.frame();
    h.frame();
    PrLayoutNode column = prLayoutNodeFirstChild(h.tree, h.view.transcript);
    const float whole = h.height(column);
    CHECK(h.entries().size() == 8);        // every item declared, and measured
    h.commandDigit(2);
    h.frame();
    h.view.scrolls[0] = 1.0e6f;             // a scroll from a layout taller than this one
    h.rounds = 1;                           // the one round that declares from that scroll
    h.commandDigit(1);
    h.rounds = 8;
    column = prLayoutNodeFirstChild(h.tree, h.view.transcript);
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 2 && h.hasText(e.back(), "line 7"));     // the spacer, and the last item
    const float shown = h.height(column);
    if (shown < whole - 0.5f || shown > whole + 0.5f) {
        std::fprintf(stderr, "  column %.1f px with every item, %.1f px from past the end\n", double(whole),
            double(shown));
    }
    CHECK(shown > whole - 0.5f && shown < whole + 0.5f);
}

// A long history scrolled up from its end: each step declares items laid out for the first time, whose estimated
// heights the next round replaces with measured ones, in the spacer above as in the items. An entry in view stays where
// it is on screen as that happens.
void scrolledUpKeepsItsPlaceAsHeightsAreMeasured() {
    Harness h;
    const std::string paragraph = "a reply long enough to wrap over a few lines of the column, so that the estimate of "
        "its height from its length is not what the layout makes of it; ";
    for (int i = 0; i < 1500; ++i) {
        std::string text = "entry " + std::to_string(i) + ": ";
        for (int p = 0; p < 1 + i % 4; ++p) {
            text += paragraph;
        }
        h.model.append(0, Entry{i % 3 == 0 ? EntryKind::Person : EntryKind::Friend, text});
    }
    h.frame();
    h.frame();
    PrRect port = h.bounds(h.view.transcript);
    PrVec2 middle{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
    h.push(pointerEvent(PrAppEventKind_PointerMove, middle));
    const auto measured = [&] {
        std::size_t n = 0;
        for (float height : h.view.items[0].height) {
            n += height > 0 ? 1 : 0;
        }
        return n;
    };
    const std::size_t atFirst = measured();
    int moved = 0;
    for (int step = 0; step < 20; ++step) {
        h.push(scrollEvent(middle, PrVec2{0, -700}));
        h.frame();
        // The spacer above stands for items not yet laid out.
        std::vector<PrLayoutNode> e = h.entries();
        CHECK(e.size() < 200);
        // A tall card can span the viewport with its top already scrolled off. Its layout must remain anchored too.
        PrLayoutNode seen = kPrLayoutNodeNone;
        for (PrLayoutNode n : e) {
            const PrRect b = h.bounds(n);
            if (b.bottom > port.top && b.top < port.bottom && prLayoutNodeFirstChild(h.tree, n) != kPrLayoutNodeNone) {
                seen = n;
                break;
            }
        }
        CHECK(seen != kPrLayoutNodeNone);
        if (seen == kPrLayoutNodeNone) {
            continue;
        }
        const float before = h.bounds(seen).top;
        h.frame();      // what the last round declared is measured, and the estimates it stood on replaced
        const float after = h.bounds(seen).top;
        if (after < before - 0.5f || after > before + 0.5f) {
            ++moved;
            std::fprintf(stderr, "  step %d: an entry in view moved from %.1f to %.1f\n", step, double(before),
                double(after));
        }
    }
    CHECK(moved == 0);
    CHECK(measured() > atFirst + 20);
}

// With `queued` prompts in the tab's queue, drawn after the transcript: the column's last item is the newest queued
// prompt if there is one, else the newest entry.
void appendsFollowAtTheBottomAndHoldAViewScrolledUp(std::size_t queued) {
    Harness h;
    // Enough entries that only those in sight and a screen either side are declared, with a spacer above them.
    constexpr int kLines = 600;
    for (int i = 0; i < kLines; ++i)
        h.model.append(0, Entry{EntryKind::Friend, "line " + std::to_string(i)});
    for (std::size_t q = 0; q < queued; ++q) {
        h.model.set_draft(0, "held " + std::to_string(q));
        h.model.queue(0);
    }
    h.frame();
    h.frame();
    PrRect port = h.bounds(h.view.transcript);
    std::vector<PrLayoutNode> e = h.entries();
    // Only the entries in sight and a screen either side are declared (view.h TranscriptItems): the newest is last
    // before the queue.
    CHECK(e.size() > queued && h.hasText(e[e.size() - 1 - queued], "line " + std::to_string(kLines - 1)));
    CHECK(e.size() < std::size_t(kLines) / 2);     // the spacers are in play
    // At the bottom: the last item shows, at the foot of the transcript.
    CHECK(!e.empty() && h.bounds(e.back()).bottom <= port.bottom + 0.5f);
    CHECK(!e.empty() && h.bounds(e.back()).bottom > port.bottom - 2 * style::kU);
    h.model.append(0, Entry{EntryKind::Friend, "newest"});
    h.frame();
    e = h.entries();
    CHECK(e.size() > queued && h.hasText(e[e.size() - 1 - queued], "newest"));
    CHECK(h.bounds(e.back()).bottom <= port.bottom + 0.5f);
    CHECK(h.bounds(e.back()).bottom > port.bottom - 2 * style::kU);
    // A new entry goes above the queued prompts; still at the bottom.
    CHECK(h.bounds(e[e.size() - 1 - queued]).bottom < h.bounds(e.back()).bottom + 0.5f);

    // Scrolled up: an entry in view stays where it is on screen while entries arrive below.
    PrVec2 middle{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
    h.push(pointerEvent(PrAppEventKind_PointerMove, middle));
    h.push(scrollEvent(middle, PrVec2{0, -400}));
    h.frame();
    h.frame();
    e = h.entries();
    PrLayoutNode seen = kPrLayoutNodeNone;
    for (PrLayoutNode n : e) {
        PrRect b = h.bounds(n);
        if (b.top > port.top && b.bottom < port.bottom) {
            seen = n;
            break;
        }
    }
    CHECK(seen != kPrLayoutNodeNone);
    CHECK(h.bounds(e.back()).top > port.bottom);       // the newest is below the view: it did scroll up
    float before = h.bounds(seen).top;
    h.model.append(0, Entry{EntryKind::Friend, "another"});
    h.model.append(0, Entry{EntryKind::Person, "and one more"});
    h.model.set_draft(0, "held later");
    h.model.queue(0);
    h.frame();
    float after = h.bounds(seen).top;
    if (after != before)
        std::fprintf(stderr, "  entry moved from %.1f to %.1f\n", double(before), double(after));
    CHECK(after > before - 0.5f && after < before + 0.5f);
}

// The harness with the sprint tab first, as the app runs, and `friends` friends f1, f2, ...
Harness withSprint(std::size_t friends = 2) {
    return Harness(friendsNamed(friends), false, true);
}

// Real key events, the production frame host and the actual Prax menu. Only the transport is fake.
void accountPickerIsAnExplicitChoice() {
    Harness h(friendsNamed(2), true, true);
    test::FakeLink link;
    FrameHost host;
    host.model = &h.model;
    host.view = &h.view;
    host.connect(&link, "test");
    h.view.selected = [&] { host.personSelected(); };
    const auto publish = [&](const std::string& account, bool automatic = false, bool supportsAuto = false) {
        auto session = std::make_shared<sync::SessionView>();
        session->id = 17;
        session->info = {"f1", "codex", account, "", ""};
        session->info.autoAccount = automatic;
        link.state.phase = sync::LinkPhase::Connected;
        link.state.loopback = true;
        link.state.sessions = sync::Sessions({session});
        link.state.sessions.setFriends({{"f1", {{"a", "Personal"}, {"b", "Work"}, {"c", "Archive", true}}, supportsAuto},
                                      {"f2"}});
        link.changed = true;
        host.pollLink();
        h.frame();
    };
    publish("b");
    h.commandDigit(2);
    h.type("keep my draft");
    const auto tabId = h.active().id;
    int signIns = 0;
    h.view.activateSignIn = [&] { ++signIns; return true; };
    link.sent.clear();
    const auto arrow = [&](bool next, PrAppModifierFlags modifiers) {
        h.key(next ? PrAppKey_Down : PrAppKey_Up, next ? PrAppKeyCode_ArrowDown : PrAppKeyCode_ArrowUp, modifiers);
        h.frame();
    };
    const auto preview = [&](const std::string& wanted, const std::string& current) {
        if (!h.view.accountPicker) return false;
        const auto& p = *h.view.accountPicker;
        return p.tab == tabId && p.session == 17 && p.current == current && p.highlighted < p.options.size() &&
               p.options[p.highlighted].name == wanted;
    };
    arrow(true, style::kCommand);
    CHECK(preview("c", "b"));
    CHECK(link.sent.empty() && signIns == 0 && h.active().id == tabId);
    CHECK(h.view.accountAnchor != kPrLayoutNodeNone && h.view.accountMenu != kPrLayoutNodeNone);
    if (h.view.accountAnchor == kPrLayoutNodeNone || h.view.accountMenu == kPrLayoutNodeNone) return;
    const PrRect tab = h.bounds(h.tabs()[h.model.active()]);
    const PrRect anchor = h.bounds(h.view.accountAnchor);
    const PrRect menu = h.bounds(h.view.accountMenu);
    CHECK(anchor.left >= tab.left - 1 && anchor.right <= tab.right + 1);
    if (menu.top < anchor.bottom - 1 || menu.top > anchor.bottom + 16)
        std::fprintf(stderr, "picker anchor top/bottom %.1f, %.1f; tab bottom %.1f; menu top/bottom %.1f, %.1f\n",
                     anchor.top, anchor.bottom, tab.bottom, menu.top, menu.bottom);
    CHECK(menu.top >= anchor.bottom - 1 && menu.top <= anchor.bottom + 16);
    CHECK(menu.left < anchor.right && menu.right > anchor.left);
    for (const char* label : {"Personal", "Work", "Archive"}) CHECK(h.hasText(h.view.accountMenu, label));
    arrow(false, 0);
    CHECK(preview("b", "b"));
    arrow(false, style::kCommand);
    CHECK(preview("a", "b") && link.sent.empty());
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    h.frame();
    CHECK(!h.view.accountPicker && h.hides == 0 && h.active().draft == "keep my draft");

    arrow(true, style::kCommand);
    CHECK(preview("c", "b") && link.sent.empty());
    h.enter();
    h.frame();
    CHECK(!h.view.accountPicker && signIns == 0 && h.active().draft == "keep my draft");
    CHECK(link.sent.size() == 1 && link.sent[0].kind == sync::ActionKind::SetAccount &&
          link.sent[0].session == 17 && link.sent[0].text == "c");
    publish("c");
    arrow(false, style::kCommand);
    CHECK(preview("b", "c"));
    h.commandDigit(3);
    h.frame();
    CHECK(h.active().name == "f2" && !h.view.accountPicker && link.sent.size() == 1);
    h.commandDigit(2);
    arrow(true, style::kCommand);
    CHECK(preview("a", "c"));
    CHECK(h.view.accountRows.size() == 3);
    if (h.view.accountRows.size() != 3) return;
    const auto point = h.centre(h.view.accountRows[1]);
    h.pointer(PrAppEventKind_PointerDown, point);
    h.pointer(PrAppEventKind_PointerUp, point);
    CHECK(!h.view.accountPicker && signIns == 0 && h.active().draft == "keep my draft");
    CHECK(link.sent.size() == 2 && link.sent[1].kind == sync::ActionKind::SetAccount && link.sent[1].text == "b");
    // With a capable server the first row becomes "auto". It remains distinct from the concrete account in use.
    publish("a", false, true);
    arrow(false, style::kCommand);
    CHECK(preview(sync::kAutoAccountTarget, "a") && link.sent.size() == 2);
    CHECK(h.view.accountRows.size() == 4);
    if (h.view.accountRows.size() != 4) return;
    CHECK(h.hasText(h.view.accountRows.front(), "auto"));
    h.enter();
    h.frame();
    CHECK(link.sent.size() == 3 && link.sent.back().text == sync::kAutoAccountTarget);
    publish("a", true, true);
    CHECK(h.hasText(h.view.accountAnchor, "Account · auto (Personal)"));
    arrow(true, style::kCommand);
    CHECK(preview("a", sync::kAutoAccountTarget));
    // Pinning the account already serving the conversation changes the selection mode.
    const auto sameAccount = h.centre(h.view.accountRows[1]);
    h.pointer(PrAppEventKind_PointerDown, sameAccount);
    h.pointer(PrAppEventKind_PointerUp, sameAccount);
    CHECK(link.sent.size() == 4 && link.sent.back().text == "a");
    publish("c", true, true);
    CHECK(h.hasText(h.view.accountAnchor, "Account · auto (Archive)"));
    CHECK(h.view.signInAccount().name == "c");

}

void fableLimitOffersKeyboardAndMouseChoices() {
    Harness h(friendsNamed(2), true, true);
    test::FakeLink link;
    FrameHost host;
    host.model = &h.model;
    host.view = &h.view;
    host.connect(&link, "test");
    h.view.selected = [&] { host.personSelected(); };
    const auto publish = [&](std::uint64_t request, std::string next, std::string model) {
        auto session = std::make_shared<sync::SessionView>();
        session->id = 19;
        session->info = {"f1", "claude", "first", "fable", ""};
        session->info.limitRequest = request;
        session->info.limitAccount = std::move(next);
        session->info.limitModel = std::move(model);
        link.state.phase = sync::LinkPhase::Connected;
        link.state.sessions = sync::Sessions({session});
        link.state.sessions.setFriends({{"f1", {{"first"}, {"second"}}}, {"f2"}});
        link.changed = true;
        host.pollLink();
        h.frame();
    };
    publish(0, "", "");
    h.commandDigit(2);
    h.type("unsent draft");
    link.sent.clear();
    publish(87, "second", "opus");
    CHECK(h.hasText(h.window(), "Out of Fable. Switch to another account or drop to Opus?"));
    CHECK(h.view.promptBox == kPrLayoutNodeNone && h.active().draft == "unsent draft");
    h.enter();
    CHECK(link.sent.empty());
    const auto old = h.view.displayedLimit;
    const auto mods = style::kCommand | PrAppModifierFlag_Shift;
    h.key(PrAppKey('O'), PrAppKeyCode_KeyO, mods);
    h.frame();
    CHECK(link.sent.size() == 1);
    if (link.sent.size() != 1) return;
    const auto& first = link.sent.front();
    CHECK(first.kind == sync::ActionKind::ResolveLimit && first.session == 19 && first.request == 87);
    CHECK(first.account == "first" && first.text == "model" && first.friendName == "opus");
    CHECK(h.hasText(h.window(), "Waiting for the server to resume the conversation…"));
    h.key(PrAppKey('A'), PrAppKeyCode_KeyA, mods);
    CHECK(link.sent.size() == 1);

    publish(88, "second", "");
    CHECK(h.hasText(h.window(), "Out of Fable. Switch to another account?"));
    h.view.resolveLimit(old, "account");
    h.key(PrAppKey('O'), PrAppKeyCode_KeyO, mods);
    CHECK(link.sent.size() == 1);
    const auto button = h.findText(h.window(), h.view.windowApp.limitAccountLabel);
    CHECK(button != kPrLayoutNodeNone);
    if (button == kPrLayoutNodeNone) return;
    const auto point = h.centre(button);
    h.pointer(PrAppEventKind_PointerDown, point);
    h.pointer(PrAppEventKind_PointerUp, point);
    CHECK(link.sent.size() == 2);
    if (link.sent.size() != 2) return;
    const auto& second = link.sent.back();
    CHECK(second.kind == sync::ActionKind::ResolveLimit && second.request == 88 &&
          second.text == "account" && second.friendName == "second" && second.account == "first");

    publish(89, "", "opus");
    CHECK(h.hasText(h.window(), "Out of Fable. Drop to Opus?"));
    h.commandDigit(3);
    h.key(PrAppKey('O'), PrAppKeyCode_KeyO, mods);
    CHECK(h.active().name == "f2" && link.sent.size() == 2);
    h.commandDigit(2);
    publish(0, "", "");
    CHECK(h.active().draft == "unsent draft" && h.view.promptBox != kPrLayoutNodeNone);
}

void arrowsFollowTheActiveTab() {
    Harness h = withSprint(2);
    std::vector<std::string> pickerCommands;
    h.view.pickerCommand = [&](const std::string& command) { pickerCommands.push_back(command); };
    auto right = [&] { h.key(PrAppKey_Right, PrAppKeyCode_ArrowRight, style::kCommand); };
    auto left = [&] { h.key(PrAppKey_Left, PrAppKeyCode_ArrowLeft, style::kCommand); };
    auto up = [&] { h.key(PrAppKey_Up, PrAppKeyCode_ArrowUp, style::kCommand); };
    auto down = [&] { h.key(PrAppKey_Down, PrAppKeyCode_ArrowDown, style::kCommand); };
    h.commandDigit(1);
    up();
    CHECK(pickerCommands.empty());
    right();
    CHECK(h.active().name == "f1");
    h.type("unsent draft");
    down();
    up();
    CHECK(pickerCommands == std::vector<std::string>({"next", "previous"}));
    CHECK(h.active().draft == "unsent draft");
    right();
    CHECK(h.active().name == "f2");
    h.commandT();
    CHECK(h.active().kind == TabKind::Terminal);
    down();
    CHECK(pickerCommands.size() == 2);
    h.model.open_extension("fixture", "Testing", "Plugin");
    h.frame();
    const auto plugin = h.model.active();
    const auto count = h.model.tab_count();
    right();
    const auto next = std::min(plugin + 1, count - 1);
    CHECK(h.model.active() == next);
    left();
    CHECK(h.model.active() == next - 1);
    h.commandDigit(1);
    left();
    CHECK(h.active().kind == TabKind::Sprint);
    h.model.select_tab(count - 1);
    h.frame();
    right();
    CHECK(h.model.active() == count - 1);
}

// ---- Terminal tabs ----

// Command-T adds a terminal tab at the end, shows it below the strip at the window's full width, with no prompt and
// no transcript; its label is empty until its shell sets a title.
void commandTAddsAndShowsATerminal() {
    Harness h;
    h.type("draft");
    h.commandT();
    CHECK(h.model.tab_count() == 3);
    CHECK(h.model.active() == 2 && h.active().kind == TabKind::Terminal && h.active().id == 2);
    CHECK(h.terminals.running.count(2) == 1);
    CHECK(!h.terminals.shown.empty() && h.terminals.shown.back() == 2);
    CHECK(h.terminals.parent == h.window());    // the terminal's view fills the room the strip leaves in the window
    CHECK(h.view.prompt == kPrLayoutNodeNone && h.view.promptBox == kPrLayoutNodeNone);
    CHECK(h.view.transcript == kPrLayoutNodeNone);
    CHECK(h.model.tab(0).draft == "draft");
    h.frame();
    std::vector<std::string> l = h.labels();
    CHECK(l.size() == 3 && l[2].empty());
    // The fake terminal draws nothing below the strip and its divider.
    PrLayoutNode window = h.window();
    const auto divider = prLayoutNodeNextSibling(h.tree, prLayoutNodeFirstChild(h.tree, window));
    CHECK(divider != kPrLayoutNodeNone);
    CHECK(prLayoutNodeNextSibling(h.tree, divider) == kPrLayoutNodeNone);
    h.terminals.titles[2] = "~/src";
    h.frame();
    l = h.labels();
    CHECK(l.size() == 3 && l[2] == "~/src");
    // Typing reaches no draft: keys on a terminal tab are the terminal's.
    h.type("ls");
    CHECK(h.model.tab(0).draft == "draft" && h.model.tab(1).draft.empty() && h.active().draft.empty());
    // Back on a friend, its prompt shows its draft again.
    h.commandDigit(1);
    CHECK(h.view.promptBox != kPrLayoutNodeNone);
    h.type("!");
    CHECK(h.model.tab(0).draft == "draft!");
}

// Command-W on a friend's tab does nothing at all, whatever terminals are open.
void commandWOnAFriendDoesNothing() {
    Harness h;
    h.type("keep");
    h.commandW();
    CHECK(h.model.tab_count() == 2 && h.model.active() == 0 && h.active().draft == "keep");
    h.commandT();
    h.commandDigit(2);
    h.commandW();
    CHECK(h.ids() == "0 1 2" && h.model.active() == 1);
    CHECK(h.terminals.closed.empty() && h.terminals.running.count(2) == 1);
    CHECK(h.hides == 0 && h.stops == 0);
}

// Command-W on a terminal's tab ends its shell and removes it: the tab selected before it shows.
void commandWClosesTheTerminal() {
    Harness h;
    h.commandT();   // 2
    h.commandT();   // 3
    h.commandT();   // 4
    CHECK(h.ids() == "0 1 2 3 4" && h.model.active() == 4);
    h.commandDigit(4);  // id 3, between two terminals
    h.commandW();
    CHECK(h.ids() == "0 1 2 4" && h.model.active() == 2 && h.active().id == 2);   // immediately left
    CHECK((h.terminals.closed == std::vector<std::size_t>{3}));
    CHECK(h.terminals.shown.back() == 2);
    h.commandW();       // no terminal left of this one
    CHECK(h.ids() == "0 1 4" && h.model.active() == 2 && h.active().id == 4);
    h.commandW();
    CHECK(h.ids() == "0 1" && h.model.active() == 0);   // alex, shown before the first terminal; not emma beside it
    CHECK((h.terminals.closed == std::vector<std::size_t>{3, 2, 4}) && h.terminals.running.empty());
    h.commandW();       // a friend: nothing
    CHECK(h.ids() == "0 1" && h.model.active() == 0);
    h.commandT();
    CHECK(h.ids() == "0 1 5");  // a new terminal takes a new id
}

// Repeated Command-W walks left, then right, then returns to the friend visited before the consoles.
void commandWDrainsTheConsoleGroup() {
    Harness h;
    for (int i = 0; i < 5; ++i) h.commandT();
    auto clickTab = [&](std::size_t index) {
        const auto at = h.centre(h.tabs()[index]);
        h.pointer(PrAppEventKind_PointerDown, at);
        h.pointer(PrAppEventKind_PointerUp, at);
        h.frame();
    };
    clickTab(1); // Emma
    clickTab(4); // middle console
    for (const auto id : {4u, 3u, 2u, 5u, 6u}) {
        CHECK(h.active().id == id);
        auto event = keyEvent(PrAppEventKind_KeyDown, 'W', PrAppKeyCode_KeyW, style::kCommand);
        event.keyboard.repeat = id != 4;
        h.push(event);
        h.frame();
    }
    h.push(keyEvent(PrAppEventKind_KeyUp, 'W', PrAppKeyCode_KeyW, style::kCommand));
    h.frame();
    h.commandW();
    CHECK(h.ids() == "0 1" && h.active().id == 1);
    CHECK((h.terminals.closed == std::vector<std::size_t>{4, 3, 2, 5, 6}));
}

// A terminal that closes while another tab shows (its shell ended) leaves the selection where it is.
void anEndedTerminalElsewhereKeepsTheSelection() {
    Harness h = withSprint();
    h.commandT();               // id 3
    h.commandDigit(2);          // f1
    h.terminals.finished.insert(3);
    h.frame();
    CHECK(h.ids() == "2 0 1" && h.active().name == "f1");
}

// On a terminal's tab, Escape asks for no hide and Command-Escape does nothing; Command-digits, the text scale's keys,
// Command-T and Command-W still work.
void escapeOnATerminalHidesNothing() {
    Harness h;
    h.commandT();
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, style::kCommand);
    h.key(PrAppKey_Enter, PrAppKeyCode_Enter);
    CHECK(h.hides == 0 && h.stops == 0);
    CHECK(h.model.active() == 2);
    h.larger();
    CHECK(h.look.textSteps == 1 && near(h.terminals.scale, style::textScale(1)));
    h.smaller();
    CHECK(h.look.textSteps == 0 && near(h.terminals.scale, 1));
    h.commandDigit(1);
    CHECK(h.model.active() == 0);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);    // a friend again: Escape hides
    CHECK(h.hides == 1);
}

// Terminals that do not show are serviced every round; the one that shows is declared instead.
void hiddenTerminalsAreServiced() {
    Harness h;
    h.commandT();   // 2
    h.commandT();   // 3, shows
    h.terminals.updates.clear();
    h.frame();
    CHECK(h.terminals.updates[2] > 0 && h.terminals.updates[3] == 0);
    h.commandDigit(1);
    h.terminals.updates.clear();
    h.frame();
    CHECK(h.terminals.updates[2] > 0 && h.terminals.updates[3] > 0);
}

// A terminal whose shell ended closes by itself, shown or not.
void anEndedTerminalCloses() {
    Harness h;
    h.commandT();   // 2
    h.commandT();   // 3, shows
    h.terminals.finished.insert(2);
    h.frame();
    CHECK(h.ids() == "0 1 3" && h.model.active() == 2 && h.active().id == 3);
    h.terminals.finished.insert(3);
    h.frame();
    CHECK(h.ids() == "0 1" && h.model.active() == 0);   // alex, selected before the terminals
    CHECK((h.terminals.closed == std::vector<std::size_t>{2, 3}));
    CHECK(h.view.promptBox != kPrLayoutNodeNone);
}

// A terminal whose shell cannot run on this machine at all (on Windows, no WSL2: terminal_launch.h) shows its one line
// in place of a shell, declares no terminal, and stays: it does not close by itself, Escape still hides nothing there,
// and Command-W closes it.
void aTerminalWithNoShellShowsItsLine() {
    Harness h;
    h.commandT();   // 2
    const std::string line = "WSL2 is not installed: wsl.exe was not found. Install it with `wsl --install` in "
        "PowerShell and restart Windows, then open a new terminal tab";
    h.terminals.problems[2] = line;
    h.terminals.shown.clear();
    h.frame();
    h.frame();
    CHECK(h.hasText(prLayoutTreeRoot(h.tree), line));
    CHECK(h.terminals.shown.empty());
    CHECK(h.ids() == "0 1 2" && h.active().id == 2);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    CHECK(h.hides == 0);
    CHECK(h.ids() == "0 1 2");
    h.commandW();
    CHECK(h.ids() == "0 1" && h.model.active() == 0);
    CHECK(!h.hasText(prLayoutTreeRoot(h.tree), line));
}

// A shell that cannot start leaves no tab, and the tab that showed before shows again.
void aTerminalThatCannotStartLeavesNoTab() {
    Harness h;
    h.terminals.refuse = true;
    h.commandT();
    CHECK(h.ids() == "0 1" && h.model.active() == 0);
    h.terminals.refuse = false;
    h.commandT();
    CHECK(h.ids() == "0 1 3");
}

// Terminals stand after the friends: a drag across the zones changes nothing; a drag among terminals reorders them;
// Command-digits follow positions.
void dragsStayInTheirZone() {
    Harness h;
    h.commandT();   // 2
    h.commandT();   // 3
    h.commandT();   // 4
    h.frame();
    h.dragTab(3, 0);        // a terminal before a friend
    CHECK(h.ids() == "0 1 2 3 4");
    h.dragTab(3, 1);        // between the friends' end and ... still a friend's place
    CHECK(h.ids() == "0 1 2 3 4");
    h.dragTab(0, 3);        // a friend among the terminals
    CHECK(h.ids() == "0 1 2 3 4");
    h.dragTab(1, 4);        // a friend to the end
    CHECK(h.ids() == "0 1 2 3 4");
    h.dragTab(4, 2);        // among the terminals
    CHECK(h.ids() == "0 1 4 2 3");
    h.dragTab(0, 1);        // among the friends
    CHECK(h.ids() == "1 0 4 2 3");
    h.commandDigit(3);
    CHECK(h.active().id == 4);
    h.commandDigit(1);
    CHECK(h.active().id == 1);
    h.commandDigit(5);
    CHECK(h.active().id == 3);
}

// ---------------------------------------------------------------------------------------------------------------------
// The sprint tab, first, as the app runs.

// The sprint tab is first, labelled "sprint", shown at launch and Command-1; the friends are Command-2 onward, and the
// terminals follow them. Its page shows, below the strip, and no prompt.
void sprintTabIsFirstAndCommandOne() {
    Harness h = withSprint();
    CHECK((h.labels() == std::vector<std::string>{"Dashboard", "f1", "f2"}));
    CHECK(h.active().kind == TabKind::Sprint);
    CHECK(h.board.shows > 0 && h.view.prompt == kPrLayoutNodeNone && h.view.promptBox == kPrLayoutNodeNone);
    CHECK(h.board.extent.x == 1000 && h.board.extent.y > 600 && h.board.extent.y < 780);
    h.commandDigit(2);
    CHECK(h.active().name == "f1" && h.view.prompt != kPrLayoutNodeNone);
    int hidden = h.board.hiddens;
    h.frame();
    CHECK(h.board.hiddens > hidden);
    h.commandT();
    CHECK(h.model.tab(3).kind == TabKind::Terminal && h.active().kind == TabKind::Terminal);
    h.commandDigit(4);
    CHECK(h.active().kind == TabKind::Terminal);
    h.commandDigit(1);
    CHECK(h.active().kind == TabKind::Sprint);
}

// Choosing the sprint tab takes its page back to the board: Command-1 from another tab and on the sprint tab itself, and
// a click on its tab, selected already or not. Another tab's key, or a click on another tab, does not.
void choosingTheSprintTabGoesHome() {
    Harness h = withSprint();
    CHECK(h.board.homes == 0);
    h.commandDigit(1);
    CHECK(h.board.homes == 1);
    h.commandDigit(2);
    CHECK(h.board.homes == 1);
    h.commandDigit(1);
    CHECK(h.board.homes == 2 && h.active().kind == TabKind::Sprint);
    PrVec2 at = h.centre(h.tabs()[0]);
    h.pointer(PrAppEventKind_PointerDown, at);
    h.pointer(PrAppEventKind_PointerUp, at);
    h.frame();
    CHECK(h.board.homes == 3 && h.active().kind == TabKind::Sprint);
    at = h.centre(h.tabs()[1]);
    h.pointer(PrAppEventKind_PointerDown, at);
    h.pointer(PrAppEventKind_PointerUp, at);
    h.frame();
    CHECK(h.board.homes == 3 && h.active().name == "f1");
    at = h.centre(h.tabs()[0]);
    h.pointer(PrAppEventKind_PointerDown, at);
    h.pointer(PrAppEventKind_PointerUp, at);
    h.frame();
    CHECK(h.board.homes == 4 && h.active().kind == TabKind::Sprint);
    h.commandDigit(2);
    h.board.ready = false;
    h.commandDigit(1);
    h.frame();
    CHECK(h.active().name == "f1" && h.view.transcript != kPrLayoutNodeNone);
    h.commandDigit(3);
    h.board.ready = true;
    h.frame();
    CHECK(h.active().name == "f2");
    h.board.ready = false;
    h.commandDigit(1);
    CHECK(h.active().name == "f2");
    h.board.ready = true;
    h.frame();
    CHECK(h.active().kind == TabKind::Sprint);
}

// Command-digit selects by strip position from the sprint tab and from a friend's, at launch and after coming back to
// the sprint tab from a friend (whose prompt had the focus); a digit past the last tab changes nothing.
void commandDigitsWorkFromTheSprintTab() {
    Harness h = withSprint(3);
    CHECK((h.labels() == std::vector<std::string>{"Dashboard", "f1", "f2", "f3"}));
    const auto positions = [&] {
        std::vector<float> left;
        const auto names = h.labels();
        const auto tabs = h.tabs();
        const auto gap = [&](std::size_t i) { return h.bounds(tabs[i]).left - h.bounds(tabs[i-1]).right; };
        CHECK(near(gap(1), gap(2)) && near(gap(2), gap(3)));
        for (std::size_t i = 0; i < names.size(); ++i)
            left.push_back(h.bounds(h.findText(h.tabs()[i], names[i])).left);
        return left;
    };
    const auto original = positions();
    h.commandDigit(3);
    CHECK(positions() == original);
    CHECK(h.active().name == "f2");
    h.commandDigit(2);
    CHECK(h.active().name == "f1");
    h.commandDigit(1);
    CHECK(h.active().kind == TabKind::Sprint);
    h.commandDigit(2);
    CHECK(h.active().name == "f1");
    h.commandDigit(1);
    CHECK(h.active().kind == TabKind::Sprint);
    h.commandDigit(3);
    CHECK(h.active().name == "f2");
    h.commandDigit(9);
    CHECK(h.active().name == "f2");
    h.commandDigit(1);
    h.commandDigit(9);
    CHECK(h.active().kind == TabKind::Sprint);
    h.commandDigit(4);
    CHECK(h.active().name == "f3");
}

// With Caps Lock on, Command-1 still selects the first tab, the sprint tab, and Command-2 the first friend: Prax's key
// bindings leave Caps Lock out of the modifiers they compare (ui.cpp 820-824), so nexus filters nothing.
void commandOneWithCapsLockSelectsTheSprintTab() {
    Harness h = withSprint();
    h.commandDigit(2, PrAppModifierFlag_CapsLock);
    CHECK(h.active().name == "f1");
    h.commandDigit(1, PrAppModifierFlag_CapsLock);
    CHECK(h.model.active() == 0 && h.active().kind == TabKind::Sprint);
}

// Exactly the active tab is drawn selected, in whichever strip it stands: the other strip shows none selected.
void onlyTheActiveTabShowsSelected() {
    Harness h = withSprint();
    // The colours as the tree holds them, in 16-bit floats.
    const PrColor chosen = prColorFromColor16(prColor16FromColor(h.kit->looks[PrKitLookId_TabChosen].text));
    const PrColor plain = prColorFromColor16(prColor16FromColor(h.kit->looks[PrKitLookId_Tab].text));
    auto same = [](PrColor a, PrColor b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
    CHECK(!same(chosen, plain));
    auto inks = [&] {
        std::string out;
        for (PrLayoutNode t : h.tabs()) {
            PrLayoutStyle record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, t));
            PrColor ink = prColorFromColor16(prLayoutColorStyleRecord(h.tree, record.groups[PrLayoutGroup_Color]).color);
            out += same(ink, chosen) ? "x" : same(ink, plain) ? "-" : "?";
        }
        return out;
    };
    h.frame();
    CHECK(inks() == "x--");
    h.commandDigit(2);
    h.frame();
    CHECK(inks() == "-x-");
    h.commandDigit(3);
    h.frame();
    CHECK(inks() == "--x");
    h.commandDigit(1);
    h.frame();
    CHECK(inks() == "x--");
}

// Typing, Enter and Command-Enter on the sprint tab change no draft and send nothing.
void sprintTabTakesNoText() {
    Harness h = withSprint();
    h.type("hello");
    h.enter();
    h.enter(style::kCommand);
    for (std::size_t i = 0; i < h.model.tab_count(); ++i) {
        CHECK(h.model.tab(i).draft.empty() && h.model.tab(i).transcript.empty() && h.model.tab(i).queue.empty());
    }
    CHECK(h.view.prompt == kPrLayoutNodeNone);
}

// The sprint tab is never carried, nothing lands before it, and the friends still reorder behind it.
void sprintTabNeverMoves() {
    Harness h = withSprint(3);
    h.dragTab(0, 2);
    CHECK(h.ids() == "3 0 1 2" && h.saves == 0);
    h.dragTab(2, 0);
    CHECK(h.model.tab(0).kind == TabKind::Sprint && h.saves == 0);
    h.dragTab(1, 3);
    CHECK(h.ids() == "3 1 2 0" && h.saves == 1);
    CHECK((h.labels() == std::vector<std::string>{"Dashboard", "f2", "f3", "f1"}));
}

// The owner's report: alex dragged to the end of the friends, the overlay hidden on the sprint tab and shown again
// comes up focused on alex. A press focuses the tab it lands on (ui.cpp PointerDown), the sprint tab has no prompt to
// take the focus back, and Escape makes the focus visible (ui.cpp routeKey): the ring stood on alex, and Enter or Space
// reaching the tree selected alex. Hiding and showing keep the selected tab, and the keyboard focus is on it.
void hideAndShowKeepTheSelectedTab() {
    Harness h(std::vector<FriendInfo>{{"alex", "m"}, {"emma", "m"}, {"f3", "m"}}, true, true);
    std::vector<PrLayoutNode> t = h.tabs();
    PrVec2 start = h.centre(t[1]);
    PrVec2 end{h.bounds(t.back()).right + 80, start.y};      // past the last tab: alex to the end
    h.pointer(PrAppEventKind_PointerDown, start);
    h.pointer(PrAppEventKind_PointerMove, PrVec2{start.x + 10, start.y});
    h.pointer(PrAppEventKind_PointerMove, end);
    h.pointer(PrAppEventKind_PointerUp, end);
    CHECK((h.labels() == std::vector<std::string>{"Dashboard", "emma", "f3", "alex"}));
    CHECK(h.active().kind == TabKind::Sprint);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);             // hide
    CHECK(h.hides == 1);
    h.frame();                                               // shown again
    h.frame();
    CHECK(h.active().kind == TabKind::Sprint);
    CHECK(prUiFocus(h.ui) != h.tabs()[3]);                   // no ring on alex
    CHECK(prUiFocus(h.ui) == h.tabs()[0]);                   // the focus is on the sprint tab
    h.key(PrAppKey(' '), PrAppKeyCode_Space, 0, " ");
    h.enter();
    CHECK(h.active().kind == TabKind::Sprint);
    // On a friend's tab the same: the friend stays selected, and the prompt has the focus.
    h.commandDigit(2);
    CHECK(h.active().name == "emma");
    h.dragTab(1, 3);                                         // emma to the end, past alex
    CHECK((h.labels() == std::vector<std::string>{"Dashboard", "f3", "alex", "emma"}));
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    h.frame();
    CHECK(h.active().name == "emma" && prUiFocus(h.ui) == h.view.prompt);
}

// A launch with a saved order that puts alex last shows the sprint tab, with no focus on alex.
void launchWithAlexLastShowsTheSprintTab() {
    std::vector<FriendInfo> loaded{{"alex", "m"}, {"emma", "m"}, {"rowan", "m"}};
    Harness h(applyTabOrder(loaded, {"rowan", "emma", "alex"}), true, true);
    CHECK((h.labels() == std::vector<std::string>{"Dashboard", "rowan", "emma", "alex"}));
    CHECK(h.active().kind == TabKind::Sprint);
    h.key(PrAppKey(' '), PrAppKeyCode_Space, 0, " ");
    CHECK(h.active().kind == TabKind::Sprint && prUiFocus(h.ui) != h.tabs()[3]);
}

// Each change of the selected tab, and only a change, asks for the selection to be saved: a drag keeps the selected tab
// and saves only the order.
void selectionChangesAreSaved() {
    Harness h = withSprint(3);
    int selections = 0;
    h.view.selected = [&selections] { ++selections; };
    h.frame();
    const int first = selections;               // the first round saves what it shows
    CHECK(first <= 1);
    h.commandDigit(3);
    CHECK(selections == first + 1);
    h.frame();
    h.dragTab(1, 3);                            // a move keeps f2 selected
    CHECK(h.active().name == "f2" && selections == first + 1 && h.saves == 1);
    h.commandDigit(1);
    CHECK(selections == first + 2);
}

// Command-W leaves the sprint tab; Escape hides the overlay there; Command-Escape stops nothing.
void sprintTabKeysCloseNothing() {
    Harness h = withSprint();
    h.commandW();
    CHECK(h.model.tab_count() == 3 && h.active().kind == TabKind::Sprint);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    CHECK(h.hides == 1);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, style::kCommand);
    CHECK(h.stops == 0);
}

// With no page to show, the tab says why: never a blank tab.
void sprintTabSaysWhyThereIsNoPage() {
    Harness h = withSprint();
    h.board.page = false;
    h.frame();
    h.frame();
    CHECK(h.hasText(prLayoutTreeRoot(h.tree), h.board.problem));
    h.view.board = nullptr;
    h.frame();
    h.frame();
    CHECK(h.hasText(prLayoutTreeRoot(h.tree), "this build has no sprint board"));
}

// The text scale's keys zoom the page.
void textKeysZoomTheBoard() {
    Harness h = withSprint();
    h.larger();
    CHECK(h.board.scale > 1);
    h.smaller();
    h.smaller();
    CHECK(h.board.scale < 1);
}

// Directory diagnostics never become a footer on the dashboard.
void dashboardKeepsRosterDiagnosticsOffscreen() {
    Harness h(std::vector<FriendInfo>{}, false, true);
    h.view.notice = "No friends to show.\nnova-config failed";
    h.frame();
    h.frame();
    CHECK(h.model.tab_count() == 1 && h.board.shows > 0);
    CHECK(!h.hasText(prLayoutTreeRoot(h.tree), h.view.notice));
}

// The dashboard waits on the server: while it has no page, the link's line shows under the board's, so a link that
// failed (here, a server whose host key changed) says why on the tab that shows, rather than waiting in silence.
void aFailedLinkShowsOnTheDashboard() {
    Harness h(std::vector<FriendInfo>{}, false, true);
    h.board.page = false;
    h.board.problem = "waiting for nexus-server to say which dashboards it offers";
    h.view.status = "glenn@studio: ssh to glenn@studio: the server's ecdsa-sha2-nistp256 host key is not the one kept";
    h.frame();
    h.frame();
    const PrLayoutNode root = prLayoutTreeRoot(h.tree);
    CHECK(h.board.shows > 0 && h.hasText(root, h.board.problem) && h.hasText(root, h.view.status));
    const std::string failed = h.view.status;
    h.view.status.clear();
    h.frame();
    CHECK(h.hasText(root, h.board.problem) && !h.hasText(root, failed));
}

// An iPad's regular width (View::columnScale, style.h kRegularWidthColumn): the prompt box and the transcript's column
// stand 10% narrower than the policy's width, centred where they stood, and their left and right edges still line up.
void regularWidthNarrowsThePromptAndTheColumn() {
    Harness h;
    h.frame();
    h.frame();
    const PrRect box = h.bounds(h.view.promptBox);
    const PrRect column = h.bounds(h.view.items[h.active().id].column);
    const auto near = [](float a, float b) { return std::fabs(a - b) <= 1.f; };
    CHECK(box.right - box.left > 100);
    CHECK(near(column.left, box.left) && near(column.right, box.right));
    h.view.columnScale = style::kRegularWidthColumn;
    h.frame();
    h.frame();
    const PrRect narrow = h.bounds(h.view.promptBox);
    const PrRect narrowColumn = h.bounds(h.view.items[h.active().id].column);
    CHECK(near(narrow.right - narrow.left, (box.right - box.left) * style::kRegularWidthColumn));
    CHECK(near(narrow.left + narrow.right, box.left + box.right));
    CHECK(near(narrowColumn.left, narrow.left) && near(narrowColumn.right, narrow.right));
    if (!near(narrow.right - narrow.left, (box.right - box.left) * style::kRegularWidthColumn) ||
        !near(narrowColumn.left, narrow.left) || !near(narrowColumn.right, narrow.right))
        std::fprintf(stderr, "prompt %g..%g -> %g..%g, column %g..%g -> %g..%g\n", box.left, box.right, narrow.left,
            narrow.right, column.left, column.right, narrowColumn.left, narrowColumn.right);
}

// An iPhone's compact width (View::fullWidthReplies): an AI's reply takes the transcript column's whole width, where
// the policy's reply_width (two thirds by default) wraps it at about half the screen. The column holds the panel's
// padding, so the whole width is the panel's inside: a reply half as wide again, and most of the column.
void compactWidthRepliesTakeTheWholeColumn() {
    Harness h;
    h.extent = h.view.extent = {402, 874};
    const std::string reply = "The nightly went red in the link walk: seed 17 loses a diff when the window reconnects "
        "while the server is still sending the state it had before the link was made again.";
    h.model.append(0, {EntryKind::Friend, reply});
    h.frame();
    h.frame();
    const auto near = [](float a, float b) { return std::fabs(a - b) <= 1.f; };
    const auto widths = [&](float* text, float* column) {
        const auto entries = h.entries();
        const PrLayoutNode body = entries.empty() ? kPrLayoutNodeNone : h.findText(entries.back(), reply);
        CHECK(body != kPrLayoutNodeNone);
        if (body == kPrLayoutNodeNone) return;
        const PrRect r = h.bounds(body);
        const PrRect c = h.bounds(h.view.items[h.active().id].column);
        *text = r.right - r.left;
        *column = c.right - c.left;
    };
    float text = 0, column = 0;
    widths(&text, &column);
    CHECK(column > 100 && text < column * 0.7f);
    h.view.fullWidthReplies = true;
    h.frame();
    h.frame();
    float fullText = 0, fullColumn = 0;
    widths(&fullText, &fullColumn);
    CHECK(near(fullColumn, column) && fullText > text * 1.4f && fullText > fullColumn * 0.8f);
    if (!(fullText > text * 1.4f && fullText > fullColumn * 0.8f))
        std::fprintf(stderr, "reply %g of column %g -> %g of %g\n", text, column, fullText, fullColumn);
}

} // namespace

// The test bundle in tests/policy/window.cmake returns one candidate appearance per active tab.
void checkTranscriptStyleValidation(nexus::policy::Host& host) {
    nexus::WindowApplication window;
    window.update(host, "friend", "", true, false, 0, 1, "verify-sign-in", {"a", "Work \"team\"", "selected@example.test"});
    CHECK(window.overlayOpacity == 0.25f && window.messageCard.emailInk == 0x557799);
    window.update(host, "friend", "", true, false, 0, 1, "verify-sign-in", {"b", "Work \"team\"", "second@example.test"});
    CHECK(window.overlayOpacity != 0.25f && window.signInAccount.name == "b");
    for (std::size_t i = 0; i < 14; ++i) {
        window.update(host, "friend", "", true, false, i, 14, "validate-style");
        CHECK((window.overlayOpacity == 0.125f) == (i < 4));
        nexus::TranscriptStyle expected;
        if (i == 2) {
            expected.ink = expected.personInk = 0xaabbcc;
            expected.radius = 0;
        } else if (i == 3) {
            expected.radius = 64;
        }
        CHECK(window.transcriptStyle == expected);
    }
}

int main(int argc, char** argv) {
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--centered-accounts") {
        Harness h = withSprint(1);
        h.view.application->watch(argv[2]);
        test::FakeLink link;
        FrameHost host;
        host.model = &h.model;
        host.view = &h.view;
        host.connect(&link, "localhost");
        const auto publish = [&](const std::string& name, uint64_t ack) {
            auto session = std::make_shared<sync::SessionView>();
            session->id = 17;
            session->info = {"f1", "codex", name, "model-a", ""};
            session->info.catalog = R"({"default":"model-a","models":{"model-a":["low","high"],"model-b":["high"]}})";
            link.state.phase = sync::LinkPhase::Connected;
            link.state.loopback = true;
            link.state.sessions = sync::Sessions({session});
            link.state.sessions.setFriends({{"f1", {{"a", "A", false, "a@example.test"},
                {"b", "B", false, "b@example.test"}, {"c", "C", true, "c@example.test"}}, true}, {"f2", {}, true}});
            link.state.sessions.setNextAction(ack);
            link.changed = true;
            host.pollLink();
            h.frame();
        };
        publish("b", 1);
        h.commandDigit(2);
        const auto beforeChoices = link.sent.size();
        h.type("/model");
        h.enter();
        h.frame();
        CHECK(h.view.choiceKind == "model" && h.view.accountRows.size() == 2 && link.sent.size() == beforeChoices);
        h.key(PrAppKey_Down, PrAppKeyCode_ArrowDown);
        h.frame();
        CHECK(h.view.choiceValue == "model-b");
        h.enter();
        h.frame();
        CHECK(h.view.choiceKind.empty() && !link.sent.empty() && link.sent.back().text == "/model model-b");
        h.type("/effort");
        h.enter();
        h.frame();
        CHECK(h.view.choiceKind == "effort" && h.view.accountRows.size() == 2);
        h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
        h.frame();
        CHECK(h.view.choiceKind.empty() && h.hides == 0 && prUiFocus(h.ui) == h.view.prompt);
        h.type("/model model-");
        h.key(PrAppKey_Tab, PrAppKeyCode_Tab);
        h.frame();
        CHECK(h.active().draft == "/model model-a");
        h.key(PrAppKey_Tab, PrAppKeyCode_Tab);
        h.frame();
        CHECK(h.active().draft == "/model model-b");
        h.enter();
        h.frame();
        CHECK(!link.sent.empty() && link.sent.back().text == "/model model-b");
        h.type("/effort h");
        h.key(PrAppKey_Tab, PrAppKeyCode_Tab);
        h.frame();
        CHECK(h.active().draft == "/effort high");
        h.enter();
        h.frame();
        CHECK(!link.sent.empty() && link.sent.back().text == "/effort high");
        CHECK(h.look.prompt.width == 98 && h.view.windowApp.centeredAccounts && h.look.transcriptStyle.glyphPeak == 3);
        link.sent.clear();
        h.type("first held");
        h.enter();
        h.frame();
        CHECK(link.sent.empty() && h.active().draft.empty() && h.active().held == "first held");
        h.type("second held");
        h.enter();
        h.frame();
        CHECK(h.active().held == "first held\nsecond held" && link.sent.empty());
        h.type("current last");
        h.key(PrAppKey_Enter, PrAppKeyCode_Enter, style::kCommand);
        h.frame();
        CHECK(link.sent.size() == 1 && link.sent[0].text == "first held\nsecond held\ncurrent last");
        h.type("another held");
        h.enter();
        h.enter();
        h.frame();
        CHECK(link.sent.size() == 2 && link.sent.back().text == "another held" && h.active().held.empty());
        h.type("first unsent");
        h.enter();
        h.type("second unsent");
        h.enter();
        h.type("keep working");
        h.key(PrAppKey_Enter, PrAppKeyCode_Enter, PrAppModifierFlag_Shift);
        h.type("second line");
        prUiTextEditSetSelection(h.ui, h.view.prompt, {3, 3});
        const auto beforeClear = link.sent.size();
        h.key(PrAppKey_Backspace, PrAppKeyCode_Backspace, style::kCommand);
        h.frame();
        CHECK(h.active().draft.empty() && h.active().held.empty() && link.sent.size() == beforeClear && prUiFocus(h.ui) == h.view.prompt);
        h.type("draft");
        link.sent.clear();
        const auto down = [&](PrAppModifierFlags mods) {
            h.key(PrAppKey_Down, PrAppKeyCode_ArrowDown, mods);
            h.frame();
        };
        down(0);
        CHECK(h.view.accountPicker && h.view.accountPicker->highlighted == 1);
        CHECK(h.hasText(h.tabs()[h.model.active()], "F1") && h.view.accountAnchor == kPrLayoutNodeNone);
        CHECK(h.view.accountRows.size() == 3);
        if (h.view.accountRows.size() != 3) return 1;
        for (unsigned i = 0; i < 3; ++i)
            CHECK(h.hasText(h.view.accountRows[i], std::string(1, 'a' + i) + "@example.test"));
        const auto menu = h.bounds(h.view.accountMenu);
        CHECK(std::abs((menu.left + menu.right) / 2 - h.extent.x / 2) < 2);
        CHECK(std::abs((menu.top + menu.bottom - h.extent.y) / 2) < 2);
        CHECK(colorMatches(fillOf(h, h.view.accountMenu), h.look.messageCard.fill));
        down(0);
        CHECK(h.view.accountPicker && h.view.accountPicker->highlighted == 2 && link.sent.empty());
        CHECK(near(h.bounds(h.view.accountMenu).left, menu.left) && near(h.bounds(h.view.accountMenu).right, menu.right));
        h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
        h.frame();
        h.type(" kept");
        CHECK(!h.view.accountPicker && h.hides == 0 && h.active().draft == "draft kept" && link.sent.empty());
        down(style::kCommand);
        down(style::kCommand);
        h.enter();
        h.frame();
        CHECK(link.sent.size() == 1 && link.sent[0].kind == sync::ActionKind::SetAccount && link.sent[0].text == "c");
        publish("c", 2);
        CHECK(link.sent.size() == 2 && link.sent.back().kind == sync::ActionKind::SignInClaim &&
              link.sent.back().account == "c" && link.sent.back().session == 17);
        CHECK(!h.view.accountPicker && h.view.accountRows.size() == 3);
        CHECK(h.hasText(h.view.accountMenu, "Please sign in to ChatGPT"));
        CHECK(h.view.transcript == kPrLayoutNodeNone);
        CHECK(h.hasText(h.view.accountMenu, "c@example.test"));
        h.view.askedTextSteps = 3;
        h.frame();
        const float lineHeight = h.kit->theme.types[PrKitTextRole_Body].lineHeight;
        CHECK(h.kit->theme.types[PrKitTextRole_Strong].weight == 700);
        const float rowOpacity = fillOf(h, h.view.accountRows[2]).a;
        CHECK(rowOpacity > 0.4f && rowOpacity < 0.85f);  // half-opacity selection plus its SDR pulse and flash
        checkTextPaint(h, "a@example.test", h.look.messageCard.ink, h.look.messageCard.fill);
        const auto card = h.bounds(h.view.accountMenu);
        CHECK(std::abs((card.top + card.bottom - h.extent.y) / 2) < 2);
        const auto selectedRow = h.bounds(h.view.accountRows[2]);
        CHECK(selectedRow.left - card.left < 0.25f * lineHeight);
        CHECK(card.right - selectedRow.right < 0.25f * lineHeight);
        const auto backdrop = h.bounds(prLayoutNodeFirstChild(h.tree, h.view.accountMenu));
        const float extra = (card.bottom - card.top) * h.look.messageCard.verticalExtension / 100;
        CHECK(near(card.top - backdrop.top, extra) && near(backdrop.bottom - card.bottom, extra));
        const float extension = (card.right - card.left) * h.look.messageCard.sideExtension / 100;
        CHECK(std::abs(card.left - backdrop.left - extension) < 2);
        CHECK(std::abs(backdrop.right - card.right - extension) < 2);

        const auto otherFriend = h.centre(h.tabs()[2]);
        const auto layer = h.bounds(prLayoutNodeParent(h.tree, h.view.accountMenu));
        CHECK(layer.top >= h.bounds(h.tabs()[2]).bottom);
        h.pointer(PrAppEventKind_PointerDown, otherFriend);
        h.pointer(PrAppEventKind_PointerUp, otherFriend);
        h.frame();
        CHECK(h.active().name == "f2" && h.view.accountMenu == kPrLayoutNodeNone);
        h.commandDigit(2);
        h.frame();
        auto previous = h.bounds(h.findText(h.view.accountMenu, "Please sign in to ChatGPT"));
        for (auto row : h.view.accountRows) {
            const auto bounds = h.bounds(row);
            CHECK(bounds.bottom - bounds.top >= 1.4f * lineHeight);
            CHECK(bounds.top - previous.bottom >= 0.1f * lineHeight);
            previous = bounds;
        }
        const auto sent = link.sent.size();
        h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
        h.frame();
        CHECK(h.view.accountMenu == kPrLayoutNodeNone && h.hides == 0 && prUiFocus(h.ui) == h.view.prompt);
        h.type(" again");
        h.enter();
        h.frame();
        CHECK(h.hasText(h.view.accountMenu, "Please sign in to ChatGPT"));
        CHECK(h.view.transcript == kPrLayoutNodeNone);
        CHECK(link.sent.size() == sent && h.active().draft == "draft kept again");
        const std::string failure = "Could not sign in to c@example.test. The signed-in email differs.";
        h.model.append(h.model.active(), {EntryKind::Notice, failure});
        h.model.append(h.model.active(), {EntryKind::Notice, "c needs signing in: try again"});
        h.frame();
        CHECK(h.hasText(h.view.accountMenu, failure));
        h.key(PrAppKey_Backspace, PrAppKeyCode_Backspace, style::kCommand);
        h.frame();
        CHECK(h.active().draft.empty() && h.view.accountMenu != kPrLayoutNodeNone);
        h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
        h.frame();
        h.type("/login");
        h.enter();
        h.frame();
        CHECK(h.active().draft.empty() && h.view.accountPicker && h.view.accountMenu != kPrLayoutNodeNone);
        CHECK(link.sent.size() == sent);
        h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
        h.frame();
        h.type("/help");
        h.enter();
        h.frame();
        CHECK(h.active().draft.empty() && link.sent.size() == sent);
        CHECK(h.view.transcript != kPrLayoutNodeNone && h.view.accountMenu == kPrLayoutNodeNone);
        CHECK(h.view.helpText.find("⌘↑ / ⌘↓") != std::string::npos);
        CHECK(h.view.helpText.find("⌘+ / ⌘−") != std::string::npos);
        CHECK(h.view.helpText.find("/login") != std::string::npos);
        CHECK(h.hasText(h.view.transcript, h.view.helpText));
        h.model.append(h.model.active(), {EntryKind::Notice, "local history"});
        h.model.set_draft(h.model.active(), "/clear");
        h.frame();
        h.enter();
        h.frame();
        CHECK(link.sent.size() == sent && h.active().draft.empty() && h.active().transcript.empty());
        CHECK(h.view.accountMenu == kPrLayoutNodeNone && prUiFocus(h.ui) == h.view.prompt);
        down(0);
        CHECK(h.view.accountPicker && h.view.accountPicker->highlighted == 2);
        if (argc == 4) {
            std::filesystem::copy_file(argv[3], argv[2], std::filesystem::copy_options::overwrite_existing);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!h.hasText(h.tabs()[h.model.active()], "Session") && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                h.frame();
            }
            CHECK(h.hasText(h.tabs()[h.model.active()], "Session"));
            h.frame();
            CHECK(h.look.tabs.fill == 0x112233 && h.look.tabs.selectedWeight == 500);
            CHECK(h.look.prompt.caretHeight == 18 && h.look.prompt.caretAspect == 0.75f && h.look.prompt.caretSeconds == 2);
            CHECK(h.look.prompt.caretCurve.size() == 4);
            CHECK(h.look.prompt.caretCurve.at(1).second == 0);
            CHECK(h.look.transcriptStyle.scannerCurve.size() == 3);
            CHECK(h.look.transcriptStyle.scannerCurve.front().second == 1);
            CHECK(h.look.transcriptStyle.scannerGap == 8 && h.look.transcriptStyle.scannerOffset == 4);
            CHECK(h.look.prompt.fill == 0x221144 && h.look.prompt.width == 72 && !h.look.prompt.showPlus);
            CHECK(h.look.messageCard.fill == 0x223344 && h.look.messageCard.selectorWidthEm == 19);
            std::string error;
            const auto hotkey = overlayBinding(*h.view.application, error);
            CHECK(error.empty() && hotkey.code == PrAppKeyCode_KeyJ && hotkey.modifiers == (style::kCommand | PrAppModifierFlag_Shift));
            CHECK(h.view.accountRows.size() == 1 && h.hasText(h.view.accountMenu, "Use my work account"));
            h.key(PrAppKey('K'), PrAppKeyCode_KeyK, style::kCommand);
            h.frame();
            CHECK(link.sent.size() == sent + 1 && link.sent.back().kind == sync::ActionKind::SetAccount &&
                  link.sent.back().text == "b");
        }
        return failures ? 1 : 0;
    }
    if (argc == 5 && std::string(argv[1]) == "--group-drag") {
        Harness h = withSprint(1);
        h.view.nativeTabs.emplace("text_editor", drawTabTextArea);
        h.view.application->watch(argv[2]);
        const auto activate = [&](const char* bundle) {
            auto before = h.view.windowApp.layoutGeneration;
            std::filesystem::copy_file(bundle, argv[2], std::filesystem::copy_options::overwrite_existing);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (h.view.windowApp.layoutGeneration == before && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                h.frame();
            }
            CHECK(h.view.windowApp.layoutGeneration != before);
        };
        activate(argv[3]);
        h.key('L', PrAppKeyCode_KeyL, style::kCommand);
        h.key('R', PrAppKeyCode_KeyR, style::kCommand);
        h.key('Y', PrAppKeyCode_KeyY, style::kCommand);
        h.commandT();
        h.frame();
        // Tools [script, terminal], Chat [friend, native], Review [sprint, second instance of same native renderer].
        CHECK(h.model.tab_count() == 6);
        if (h.model.tab_count() != 6) return 1;
        CHECK(h.ids() == "4 5 0 2 1 3");
        const auto active = h.active().id;
        h.dragTab(5, 0);
        h.dragTab(0, 5);
        CHECK(h.ids() == "4 5 0 2 1 3" && h.active().id == active);
        h.dragTab(3, 2);
        CHECK(h.ids() == "4 5 2 0 1 3");
        const auto point = h.centre(h.tabs()[5]);
        h.pointer(PrAppEventKind_PointerDown, point);
        h.pointer(PrAppEventKind_PointerUp, point);
        CHECK(h.active().type == "right");
        // Move and select entirely by keyboard. The default fence rejects a step into the next group.
        const auto reorder = PrAppModifierFlags(style::kCommand | PrAppModifierFlag_Shift);
        h.key(PrAppKey_Left, PrAppKeyCode_ArrowLeft, reorder);
        CHECK(h.ids() == "4 5 2 0 3 1" && h.active().type == "right");
        h.key(PrAppKey_Left, PrAppKeyCode_ArrowLeft, reorder);
        CHECK(h.ids() == "4 5 2 0 3 1" && h.active().type == "right");
        h.key(PrAppKey_Right, PrAppKeyCode_ArrowRight, reorder);
        CHECK(h.ids() == "4 5 2 0 1 3" && h.active().type == "right");
        activate(argv[4]);
        h.dragTab(5, 0);
        CHECK(h.ids() == "3 4 5 2 0 1" && h.active().group == "Tools" && h.active().type == "right");
        h.dragTab(0, 5);  // the other direction is still forbidden
        CHECK(h.ids() == "3 4 5 2 0 1");
        activate(argv[3]);
        CHECK(h.active().group == "Review" && h.active().type == "right");
        CHECK(h.ids() == "4 5 2 0 3 1");
        h.commandDigit(1);  // select across groups, then reorder and close using only the keys
        h.commandDigit(5);
        h.key(PrAppKey_Right, PrAppKeyCode_ArrowRight, reorder);
        CHECK(h.active().type == "right" && h.model.active() == 5);
        h.key('W', PrAppKeyCode_KeyW, style::kCommand);
        CHECK(h.model.tab_count() == 5 && h.active().type == "words");
        return failures ? 1 : 0;
    }
    if (argc == 4 && std::string(argv[1]) == "--group-close") {
        auto h = withSprint();
        h.view.application->watch(argv[2]);
        h.commandW();
        CHECK(h.active().kind == TabKind::Sprint && h.model.tab_count() == 3);
        h.commandDigit(2);
        h.commandW();
        CHECK(h.active().kind == TabKind::Friend && h.model.tab_count() == 3);
        h.commandT();
        const auto first = h.active().id;
        h.commandT();
        h.commandW();
        CHECK(h.active().id == first && h.model.tab_count() == 4);
        CHECK(h.terminals.closed.size() == 1);
        std::filesystem::copy_file(argv[3], argv[2]);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (h.view.windowApp.closable.value_or(true) && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            h.frame();
        }
        h.commandW();
        CHECK(h.active().id == first && h.terminals.running.count(first) && h.terminals.closed.size() == 1);
        h.commandDigit(1);
        h.commandW();
        CHECK(h.model.tab_count() == 3 && h.active().id == first);
        h.commandDigit(1);
        h.commandW();
        CHECK(h.model.tab_count() == 2 && h.active().id == first);
        h.key('P', PrAppKeyCode_KeyP, style::kCommand);
        CHECK(h.active().type == "pinned");
        h.commandW();
        CHECK(h.active().type == "pinned" && h.model.tab_count() == 3);
        return failures ? 1 : 0;
    }
    if (argc == 3 || argc == 4) {
        Harness h;
        CHECK(h.view.nativeTabs.empty());
        h.view.nativeTabs.emplace("text_editor", drawTabTextArea);
        h.view.application->watch(argv[1]);
        h.type("before");
        h.enter();
        CHECK(h.active().transcript.size() == 1);
        std::filesystem::copy_file(argv[2], argv[1]);
        // Wait for the running window to observe the atomic activation.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!h.view.windowApp.types.count("editor") && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            h.frame();
        }
        CHECK(near(h.view.windowApp.overlayOpacity * 100, 35.0f));
        h.frame();
        CHECK(h.look.messageCard.fill == 0xedf2ea && h.look.messageCard.ink == 0x192218);
        CHECK(h.look.messageCard.radius == 24 && h.kit->theme.types[PrKitTextRole_Display].size == 38);
        CHECK(h.kit->theme.palette.bg0.a == 1);   // ordinary window
        h.look.overlay = true;
        prUiForget(h.ui, &h.look);
        h.frame();
        CHECK(near(h.kit->theme.palette.bg0.a * 100, 35.0f));
        CHECK(near(fillOf(h, h.window()).a * 100, 35.0f));
        CHECK(h.kit->theme.palette.text1.a == 1);
        std::string accountCommand;
        h.view.accountCommand = [&](const std::string& name) { accountCommand = name; };
        h.key(PrAppKey_Up, PrAppKeyCode_ArrowUp, style::kCommand);
        CHECK(accountCommand == "custom");
        h.type("after");
        h.enter();
        CHECK(h.active().transcript.size() == 1);
        CHECK((h.active().queue == std::vector<std::string>{"edited after"}));
        h.frame();
        h.key('K', PrAppKeyCode_KeyK, style::kCommand);
        CHECK(h.view.askedTextSteps == 2);
        h.key('E', PrAppKeyCode_KeyE, style::kCommand);
        CHECK(h.active().kind == TabKind::Extension && h.active().type == "editor" && h.active().group == "Writing");
        h.type("native buffer");
        CHECK(h.active().draft == "native buffer");
        std::string service, payload;
        h.view.requestService = [&](const std::string& target, const std::string& text) {
            service = target;
            payload = text;
            return std::string();
        };
        h.key(PrAppKey_Enter, PrAppKeyCode_Enter, style::kCommand);
        CHECK(service == "text-tools" && payload == "native buffer" && h.active().draft == payload);
        h.key('W', PrAppKeyCode_KeyW, PrAppModifierFlags(style::kCommand | PrAppModifierFlag_Shift));
        CHECK(h.active().type == "word-counter" && h.active().group == "Tools");
        h.type("script buffer");
        CHECK(h.active().draft == "script buffer");
        CHECK(!h.model.move_tab(h.model.active(), h.model.active() - 1));
        h.key(PrAppKey_Enter, PrAppKeyCode_Enter, style::kCommand);
        CHECK(service == "word-count" && payload == "script buffer");
        h.key('W', PrAppKeyCode_KeyW, style::kCommand);
        CHECK(h.active().type == "editor" && h.active().draft == "native buffer");
        CHECK(h.view.windowApp.autoAccountLabel == "rotate");
        checkTranscriptStyleValidation(*h.view.application);
        if (argc == 4) {
            // The new harness starts on stock, then reads the live fork. Entries remain in the model across each reload.
            Harness styled;
            styled.view.accountCaption = [] { return "Fixture account"; };
            styled.model.append(0, {EntryKind::Person, "Fixture question"});
            styled.model.append(0, {EntryKind::Friend, "Fixture chat"});
            styled.model.set_draft(0, "Fixture queued");
            styled.model.queue(0);
            styled.model.append(0, {EntryKind::Notice, "Fixture notice"});
            styled.model.append(0, {EntryKind::Tool, "Bash: git status"});
            styled.frame();
            styled.frame();
            CHECK(styled.hasText(styled.window(), "Fixture notice") && styled.hasText(styled.window(), "Bash: git status"));
            const std::string styleFile = std::string(argv[1]) + ".styles";
            styled.view.application->watch(styleFile);
            std::filesystem::copy_file(argv[2], styleFile);
            const auto waitForStyle = [&](bool notices) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                do {
                    styled.frame();
                    const auto& colors = styled.view.windowApp.transcriptStyle;
                    if (colors.fill == 0x102030 && colors.showNotices == notices && colors.showTools == notices) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } while (std::chrono::steady_clock::now() < until);
                styled.frame();
                styled.frame();
                const auto& colors = styled.view.windowApp.transcriptStyle;
                CHECK(colors.fill == 0x102030 && colors.showNotices == notices && colors.showTools == notices);
            };
            waitForStyle(false);
            checkTextPaint(styled, "Fixture question", 0x9a9a9a, 0x102030);
            checkTextPaint(styled, "Fixture queued", 0x9a9a9a, 0x102030);
            checkTextPaint(styled, "Fixture chat", 0xf0e0d0, styled.look.tabs.fill);
            CHECK(styled.view.accountAnchor && styled.hasText(styled.view.accountAnchor, "Fixture account"));
            checkTextPaint(styled, "Fixture account", 0xcfdfa0, 0x102030);
            CHECK(!styled.hasText(styled.window(), "Fixture notice") && !styled.hasText(styled.window(), "Bash: git status"));
            std::filesystem::copy_file(argv[3], styleFile, std::filesystem::copy_options::overwrite_existing);
            waitForStyle(true);
            CHECK(styled.hasText(styled.window(), "Fixture notice") && !styled.hasText(styled.window(), "Bash: git status"));
            checkTextPaint(styled, "$ git status", 0xdc7334, 0x102030);
            checkTextPaint(styled, "Fixture notice", 0xffa0a0, 0x182134);
            CHECK(styled.active().transcript.size() == 4 && styled.entries().size() == 5);
            styled.view.promptOpen = false;
            styled.view.status = "Fixture transport status";
            styled.frame();
            styled.frame();
            CHECK(styled.view.windowApp.transcriptStyle.fill == 0x102030);
            CHECK(styled.look.transcriptStyle.fill == 0x102030);
            checkTextPaint(styled, "Fixture transport status", 0xcfdfa0, 0x102030);
        }
        return failures ? 1 : 0;
    }
    sprintTabIsFirstAndCommandOne();
    choosingTheSprintTabGoesHome();
    commandDigitsWorkFromTheSprintTab();
    commandOneWithCapsLockSelectsTheSprintTab();
    arrowsFollowTheActiveTab();
    accountPickerIsAnExplicitChoice();
    fableLimitOffersKeyboardAndMouseChoices();
    onlyTheActiveTabShowsSelected();
    sprintTabTakesNoText();
    sprintTabNeverMoves();
    hideAndShowKeepTheSelectedTab();
    launchWithAlexLastShowsTheSprintTab();
    selectionChangesAreSaved();
    sprintTabKeysCloseNothing();
    sprintTabSaysWhyThereIsNoPage();
    textKeysZoomTheBoard();
    dashboardKeepsRosterDiagnosticsOffscreen();
    regularWidthNarrowsThePromptAndTheColumn();
    compactWidthRepliesTakeTheWholeColumn();
    aFailedLinkShowsOnTheDashboard();
    typingFillsTheActiveDraft();
    enterSendsExactlyTheText();
    logoutRequiresAnExplicitEnterAndNeverBecomesChat();
    enterStartsSignInBeforeEditingTheDraft();
    commandEnterQueues();
    shiftEnterInsertsOneLineFeed();
    blankDraftsSendNothing();
    commandDigitSelectsATabAndTypesNothing();
    commandZeroSelectsTheTenthTab();
    capsLockKeepsTheBindings();
    eachTabKeepsItsDraft();
    enterWhileComposingCommitsOnly();
    textKeysScaleTheTranscriptAndThePrompt();
    textScaleIsClamped();
    textScaleChangesAreSaved();
    textKeysWorkWithCapsLock();
    escapeHidesAndCommandEscapeStops();
    escapeWindowedHidesNothing();
    escapeWhileComposingDoesNotHide();
    dragMovesATab();
    clickBelowThresholdSelects();
    clickSelectsATab();
    savedOrderOnRelaunch();
    escapeCancelsADrag();
    draftAndScrollStayWithTheirFriend();
    dragWithOneTabDoesNothing();
    insertionCaretAndPromptMargins();
    emptyPromptAndTranscriptShowNoText();
    promptIsARoundedBoxHoldingThePlus();
    promptSegmentTracksInsertionAndUsesHeadroom();
    newGlyphsFadeWithoutRelightingOldText();
    noModelUnderThePrompt();
    switchNoticeBubble();
    namedBusBubbles();
    copyMarkdownConversation();
    markdownListsAndLinks();
    longInlineCommandWrapsInTheReply();
    conversationBubblesAndSignInCard();
    overlayKeepsAllButTheBackgroundOpaque();
    longUnbrokenTextWraps();
    noFriendsShowsTheNotice();
    primaryChatUsesTheOrdinaryComposer();
    longTranscriptDeclaresWhatIsInSight();
    promptCutKeepsWholeCharacters();
    promptRoomCountsTheSelection();
    cutLineStaysWithItsTabAndDraft();
    scrollPastTheEndCountsTheLastItemOnce();
    scrolledUpKeepsItsPlaceAsHeightsAreMeasured();
    appendsFollowAtTheBottomAndHoldAViewScrolledUp(0);
    appendsFollowAtTheBottomAndHoldAViewScrolledUp(3);
    toolLinesAreOneCompactLine();
    growingTextShowsAfterTheTranscript();
    stateCursorUnderTheLastText();
    nativeBusSendIsPrivateInHistory();
    scannerHasPromptPadding();
    sendingKeepsBubbleOnScannerRow();
    nothingAroundTheOpenPrompt();
    commandTAddsAndShowsATerminal();
    commandWOnAFriendDoesNothing();
    commandWClosesTheTerminal();
    commandWDrainsTheConsoleGroup();
    anEndedTerminalElsewhereKeepsTheSelection();
    escapeOnATerminalHidesNothing();
    hiddenTerminalsAreServiced();
    anEndedTerminalCloses();
    aTerminalThatCannotStartLeavesNoTab();
    aTerminalWithNoShellShowsItsLine();
    dragsStayInTheirZone();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window input tests passed\n");
    return 0;
}
