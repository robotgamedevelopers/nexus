// The window's input, headless: the nexus view built over a Prax UI context and Studio Kit with no window and no GPU,
// fed events as the window would feed them, with the model checked after each. Frames carry a
// made-up clock (from 1000 s), never the real one. Text is set in the system's fonts, so these are functional tests.

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_surface.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"

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

    bool show(PrLayoutNode, PrVec2 e, std::string* why) override {
        ++shows;
        extent = e;
        if (!page)
            *why = problem;
        return page;
    }
    void hidden() override { ++hiddens; }
    void setTextScale(float k) override { scale = k; }
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
    double time = 1000;
    int hides = 0;      // hide requests reaching the host seam (View::hide), which the overlay connects to hideOverlay
    int stops = 0;      // stop requests (View::stop)
    int saves = 0;      // tab order saves asked for after a move (View::moved)

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
        PrUiFrameDesc desc{
            .time = time,
            .delta = 1.0 / 60,
            .extent = PrVec2{1000, 780},
            .maxRounds = 8,
            .display = prAppDisplayDefault(),
            .preferences = {},
        };
        // Under a surface that fills the viewport, as Prax's studio window builds the app's round (studio_window.cpp
        // build), so that the window is not the tree's root, as in the app.
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Harness& h = *static_cast<Harness*>(context);
            PrKitSurfaceStyle style{
                .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
                .background = PrKitBackground_App,
            };
            buildWindow(h.view, h.kit, prKitSurface(h.kit, kPrLayoutNodeNone, {}, style));
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
        key(PrAppKey('0' + digit), code, PrAppModifierFlags(PrAppModifierFlag_Super | extra));
    }

    const Tab& active() const {
        return model.tab(model.active());
    }

    // The transcript's entries, at the last layout: the children of its column.
    std::vector<PrLayoutNode> entries() const {
        std::vector<PrLayoutNode> out;
        PrLayoutNode column = prLayoutNodeFirstChild(tree, view.transcript);
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
    // With the sprint tab, the window's first child is a row of two strips: the sprint tab's, then the others'.
    std::vector<PrLayoutNode> tabs() const {
        std::vector<PrLayoutNode> out;
        const bool sprint = model.tab_count() != 0 && model.tab(0).kind == TabKind::Sprint;
        std::vector<PrLayoutNode> strips;
        if (sprint) {
            for (PrLayoutNode s = prLayoutNodeFirstChild(tree, strip()); s != kPrLayoutNodeNone;
                    s = prLayoutNodeNextSibling(tree, s))
                strips.push_back(s);
        } else {
            strips.push_back(strip());
        }
        for (PrLayoutNode s : strips) {
            for (PrLayoutNode n = prLayoutNodeFirstChild(tree, s); n != kPrLayoutNodeNone;
                    n = prLayoutNodeNextSibling(tree, n))
                out.push_back(n);
        }
        return out;
    }

    // True if some text under `node` at the last layout is `text`.
    bool hasText(PrLayoutNode node, const std::string& text) const {
        PrStringView t = prLayoutNodeText(tree, node);
        if (t.data != nullptr && std::string(t.data, t.size) == text)
            return true;
        for (PrLayoutNode n = prLayoutNodeFirstChild(tree, node); n != kPrLayoutNodeNone;
                n = prLayoutNodeNextSibling(tree, n)) {
            if (hasText(n, text))
                return true;
        }
        return false;
    }
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
        key(PrAppKey('T'), PrAppKeyCode_KeyT, PrAppModifierFlag_Super);
    }
    void commandW() {
        key(PrAppKey('W'), PrAppKeyCode_KeyW, PrAppModifierFlag_Super);
    }

    // The tab labels' texts, in order.
    std::vector<std::string> labels() const {
        std::vector<std::string> out;
        for (PrLayoutNode t : tabs()) {
            PrLayoutNode only = prLayoutNodeFirstChild(tree, t);
            PrStringView text = only != kPrLayoutNodeNone ? prLayoutNodeText(tree, only) : PrStringView{};
            out.emplace_back(text.data != nullptr ? text.data : "", text.size);
        }
        return out;
    }

    // Command-= (larger), Command-+ (Shift-=, larger) and Command-minus (smaller), as macOS sends them: no text. Then a
    // frame more, whose start applies the scale.
    void larger(PrAppModifierFlags extra = 0) {
        key(PrAppKey('='), PrAppKeyCode_Equal, PrAppModifierFlags(PrAppModifierFlag_Super | extra));
        frame();
    }
    void largerWithShift() {
        key(PrAppKey('+'), PrAppKeyCode_Equal, PrAppModifierFlags(PrAppModifierFlag_Super | PrAppModifierFlag_Shift));
        frame();
    }
    void smaller(PrAppModifierFlags extra = 0) {
        key(PrAppKey('-'), PrAppKeyCode_Minus, PrAppModifierFlags(PrAppModifierFlag_Super | extra));
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

void commandEnterQueues() {
    Harness h;
    h.type("later");
    h.enter(PrAppModifierFlag_Super);
    CHECK((h.active().queue == std::vector<std::string>{"later"}));
    CHECK(h.active().transcript.empty());     // the queue, not the transcript
    CHECK(h.active().draft.empty());
    CHECK(h.model.tab(1).queue.empty());
    h.type("then this");
    h.enter(PrAppModifierFlag_Super);
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
    h.enter(PrAppModifierFlag_Super);
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
    h.enter(PrAppModifierFlags(PrAppModifierFlag_Super | PrAppModifierFlag_CapsLock));
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
    h.enter(PrAppModifierFlag_Super);
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
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, PrAppModifierFlag_Super);
    CHECK(h.hides == 2 && h.stops == 1);
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, PrAppModifierFlags(PrAppModifierFlag_Super | PrAppModifierFlag_CapsLock));
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
        PrStringView text = prLayoutNodeText(h.tree, only);
        CHECK(std::string(text.data, text.size) == h.model.tab(j).name);
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
// and the text area inside it, the plus at its left, and nothing after the area.
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
    PrLayoutNode plus = prLayoutNodeFirstChild(h.tree, box);
    CHECK(plus != kPrLayoutNodeNone && plus != h.view.prompt);
    CHECK(prLayoutNodeNextSibling(h.tree, plus) == h.view.prompt);
    CHECK(prLayoutNodeNextSibling(h.tree, h.view.prompt) == kPrLayoutNodeNone);
    PrRect p = h.bounds(plus);
    PrRect area = h.bounds(h.view.prompt);
    CHECK(inside(p, outer));
    CHECK(inside(area, outer));
    CHECK(p.right <= area.left);
    CHECK(p.left > outer.left + 0.5f * style::kPromptPadX * k);
    // One line: the box is kPromptHeight tall. It is kPromptPercent of the window wide, centred, and the transcript's
    // column kColumnPercent.
    CHECK(near(outer.bottom - outer.top, style::kPromptHeight * k));
    PrRect window = h.bounds(prLayoutNodeParent(h.tree, h.view.transcript));
    const float width = window.right - window.left;
    CHECK(near(outer.right - outer.left, width * style::kPromptPercent / 100));
    CHECK(near(outer.left - window.left, window.right - outer.right));
    PrRect column = h.bounds(prLayoutNodeFirstChild(h.tree, h.view.transcript));
    CHECK(near(column.right - column.left, width * style::kColumnPercent / 100));
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
PrColor borderOf(const Harness& h, PrLayoutNode node, PrRectSide side) {
    PrLayoutStyle record = prLayoutStyleRecord(h.tree, prLayoutNodeStyle(h.tree, node));
    return prLayoutDecorationStyleRecord(h.tree, record.groups[PrLayoutGroup_Decoration]).borderColor[side];
}

// In the see-through overlay only the window background and the tab strip's fill let what is behind show through:
// the prompt box, its border, the bubbles (the queued one too) and all text are opaque.
void overlayKeepsAllButTheBackgroundOpaque() {
    Harness h(std::vector<FriendInfo>{FriendInfo{"alex", "m1"}, FriendInfo{"emma", "m2"}}, true);
    h.type("sent");
    h.enter();
    h.type("held");
    h.enter(PrAppModifierFlag_Super);
    h.frame();
    PrLayoutNode window = prLayoutNodeParent(h.tree, h.view.transcript);
    CHECK(near(fillOf(h, window).a * 100, style::kOverlayOpacity * 100));
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
    const PrKitPalette& p = h.kit->theme.palette;
    for (PrColor c : {p.text1, p.text2, p.text3, p.text4, p.textSelection})
        CHECK(c.a == 1);
    for (PrKitLookId id : {PrKitLookId_Tab, PrKitLookId_TabChosen})
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

// With `queued` prompts in the tab's queue, drawn after the transcript: the column's last item is the newest queued
// prompt if there is one, else the newest entry.
void appendsFollowAtTheBottomAndHoldAViewScrolledUp(std::size_t queued) {
    Harness h;
    for (int i = 0; i < 30; ++i)
        h.model.append(0, Entry{EntryKind::Friend, "line " + std::to_string(i)});
    for (std::size_t q = 0; q < queued; ++q) {
        h.model.set_draft(0, "held " + std::to_string(q));
        h.model.queue(0);
    }
    h.frame();
    h.frame();
    PrRect port = h.bounds(h.view.transcript);
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 30 + queued);
    // At the bottom: the last item shows, at the foot of the transcript.
    CHECK(!e.empty() && h.bounds(e.back()).bottom <= port.bottom + 0.5f);
    CHECK(!e.empty() && h.bounds(e.back()).bottom > port.bottom - 2 * style::kU);
    h.model.append(0, Entry{EntryKind::Friend, "newest"});
    h.frame();
    e = h.entries();
    CHECK(e.size() == 31 + queued && h.bounds(e.back()).bottom <= port.bottom + 0.5f);
    CHECK(e.size() == 31 + queued && h.bounds(e.back()).bottom > port.bottom - 2 * style::kU);
    // A new entry goes above the queued prompts; still at the bottom.
    CHECK(e.size() == 31 + queued && h.bounds(e[30]).bottom < h.bounds(e.back()).bottom + 0.5f);

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

// ---- Terminal tabs ----

// Command-T adds a terminal tab at the end, shows it below the strip at the window's full width, with no prompt and
// no transcript; its label is "terminal" until its shell sets a title.
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
    CHECK(l.size() == 3 && l[2] == "terminal");
    // The window holds only the strip: the fake draws nothing, and nothing else is declared.
    PrLayoutNode window = h.window();
    CHECK(prLayoutNodeNextSibling(h.tree, prLayoutNodeFirstChild(h.tree, window)) == kPrLayoutNodeNone);
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
    CHECK(h.ids() == "0 1 2 4" && h.model.active() == 3 && h.active().id == 4);   // selected before it
    CHECK((h.terminals.closed == std::vector<std::size_t>{3}));
    CHECK(h.terminals.shown.back() == 4);
    h.commandW();       // the last tab
    CHECK(h.ids() == "0 1 2" && h.model.active() == 2 && h.active().id == 2);
    h.commandW();
    CHECK(h.ids() == "0 1" && h.model.active() == 0);   // alex, shown before the first terminal; not emma beside it
    CHECK((h.terminals.closed == std::vector<std::size_t>{3, 4, 2}) && h.terminals.running.empty());
    h.commandW();       // a friend: nothing
    CHECK(h.ids() == "0 1" && h.model.active() == 0);
    h.commandT();
    CHECK(h.ids() == "0 1 5");  // a new terminal takes a new id
}

// Command-W goes back to the tab selected before the one it closes, out of every friend, terminal and the sprint tab,
// most recent first, skipping tabs closed since.
void commandWGoesBackToThePreviousTab() {
    Harness h = withSprint();   // sprint 2, f1 0, f2 1
    h.commandT();               // terminal 1: id 3, at 3
    h.commandT();               // terminal 2: id 4, at 4
    h.commandDigit(2);          // f1
    h.commandDigit(4);          // terminal 1
    h.commandDigit(1);          // sprint
    h.commandDigit(5);          // terminal 2
    h.commandW();
    CHECK(h.ids() == "2 0 1 3" && h.active().kind == TabKind::Sprint);   // not f2 nor terminal 1, beside it
    CHECK(h.board.shows > 0 && h.view.promptBox == kPrLayoutNodeNone);
    h.commandDigit(4);          // terminal 1
    h.commandW();
    CHECK(h.ids() == "2 0 1" && h.active().kind == TabKind::Sprint);     // the sprint tab, selected last before it
    h.commandT();               // id 5
    h.commandT();               // id 6
    h.commandDigit(3);          // f2
    h.commandDigit(4);          // id 5
    h.commandDigit(5);          // id 6
    h.commandDigit(4);          // id 5 again: it moves to the front of the history
    h.commandW();
    CHECK(h.ids() == "2 0 1 6" && h.active().id == 6);                   // id 6, selected before id 5
    h.commandW();
    CHECK(h.ids() == "2 0 1" && h.active().name == "f2");                 // f2, selected before id 6 (id 5 closed)
    CHECK((h.terminals.closed == std::vector<std::size_t>{4, 3, 5, 6}) && h.terminals.running.empty());
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
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, PrAppModifierFlag_Super);
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
    CHECK((h.labels() == std::vector<std::string>{"sprint", "f1", "f2"}));
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

// Command-digit selects by strip position from the sprint tab and from a friend's, at launch and after coming back to
// the sprint tab from a friend (whose prompt had the focus); a digit past the last tab changes nothing.
void commandDigitsWorkFromTheSprintTab() {
    Harness h = withSprint(3);
    CHECK((h.labels() == std::vector<std::string>{"sprint", "f1", "f2", "f3"}));
    h.commandDigit(3);
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
    h.enter(PrAppModifierFlag_Super);
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
    CHECK((h.labels() == std::vector<std::string>{"sprint", "f2", "f3", "f1"}));
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
    CHECK((h.labels() == std::vector<std::string>{"sprint", "emma", "f3", "alex"}));
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
    CHECK((h.labels() == std::vector<std::string>{"sprint", "f3", "alex", "emma"}));
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape);
    h.frame();
    CHECK(h.active().name == "emma" && prUiFocus(h.ui) == h.view.prompt);
}

// A launch with a saved order that puts alex last shows the sprint tab, with no focus on alex.
void launchWithAlexLastShowsTheSprintTab() {
    std::vector<FriendInfo> loaded{{"alex", "m"}, {"emma", "m"}, {"rowan", "m"}};
    Harness h(applyTabOrder(loaded, {"rowan", "emma", "alex"}), true, true);
    CHECK((h.labels() == std::vector<std::string>{"sprint", "rowan", "emma", "alex"}));
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
    h.key(PrAppKey_Escape, PrAppKeyCode_Escape, PrAppModifierFlag_Super);
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

// With no friends, the sprint tab still shows, and the reason there are none is beneath it.
void sprintTabWithoutFriendsShowsTheNotice() {
    Harness h(std::vector<FriendInfo>{}, false, true);
    h.view.notice = "No friends to show.\nnova-config failed";
    h.frame();
    h.frame();
    CHECK(h.model.tab_count() == 1 && h.board.shows > 0);
    CHECK(h.hasText(prLayoutTreeRoot(h.tree), h.view.notice));
}

} // namespace

int main() {
    sprintTabIsFirstAndCommandOne();
    commandDigitsWorkFromTheSprintTab();
    commandOneWithCapsLockSelectsTheSprintTab();
    onlyTheActiveTabShowsSelected();
    sprintTabTakesNoText();
    sprintTabNeverMoves();
    hideAndShowKeepTheSelectedTab();
    launchWithAlexLastShowsTheSprintTab();
    selectionChangesAreSaved();
    sprintTabKeysCloseNothing();
    sprintTabSaysWhyThereIsNoPage();
    textKeysZoomTheBoard();
    sprintTabWithoutFriendsShowsTheNotice();
    typingFillsTheActiveDraft();
    enterSendsExactlyTheText();
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
    emptyPromptAndTranscriptShowNoText();
    promptIsARoundedBoxHoldingThePlus();
    noModelUnderThePrompt();
    overlayKeepsAllButTheBackgroundOpaque();
    longUnbrokenTextWraps();
    noFriendsShowsTheNotice();
    appendsFollowAtTheBottomAndHoldAViewScrolledUp(0);
    appendsFollowAtTheBottomAndHoldAViewScrolledUp(3);
    commandTAddsAndShowsATerminal();
    commandWOnAFriendDoesNothing();
    commandWClosesTheTerminal();
    commandWGoesBackToThePreviousTab();
    anEndedTerminalElsewhereKeepsTheSelection();
    escapeOnATerminalHidesNothing();
    hiddenTerminalsAreServiced();
    anEndedTerminalCloses();
    aTerminalThatCannotStartLeavesNoTab();
    dragsStayInTheirZone();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window input tests passed\n");
    return 0;
}
