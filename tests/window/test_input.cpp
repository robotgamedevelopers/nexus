// The window's input, headless: the nexus view built over a Prax UI context and Studio Kit with no window and no GPU,
// fed events as the window would feed them (through takeEvent), with the model checked after each. Frames carry a
// made-up clock (from 1000 s), never the real one. Text is set in the system's fonts, so these are functional tests.

#include <cstdio>
#include <string>
#include <vector>

#include "pr/font_set.h"
#include "pr/kit.h"
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

std::vector<FriendInfo> friendsNamed(std::size_t count) {
    std::vector<FriendInfo> out;
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(FriendInfo{"f" + std::to_string(i + 1), "m"});
    return out;
}

// A window with no window: the view, its model of two friends (or as many as asked), and a kit over a UI context over
// a layout tree.
struct Harness {
    Model model;
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    Look look;
    View view;
    double time = 1000;
    int hides = 0;      // hide requests reaching the host seam (View::hide), which the overlay connects to hideOverlay
    int stops = 0;      // stop requests (View::stop)
    int saves = 0;      // tab order saves asked for after a move (View::moved)

    Harness() : Harness(std::vector<FriendInfo>{FriendInfo{"alex", "m1"}, FriendInfo{"emma", "m2"}}) {}

    // With `overlay`, the window is drawn as the see-through overlay is.
    explicit Harness(const std::vector<FriendInfo>& friends, bool overlay = false) : model(friends) {
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
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Harness& h = *static_cast<Harness*>(context);
            buildWindow(h.view, h.kit, kPrLayoutNodeNone);
        }, this);
    }

    // Queues an event as the window does: through the event filter first.
    void push(PrAppEvent e) {
        e.time = time;
        if (!takeEvent(ui, e))
            prUiPushEvent(ui, &e);
    }

    void key(PrAppKey k, PrAppKeyCode code, PrAppModifierFlags modifiers = 0, const char* text = nullptr) {
        PrStringView typed = text != nullptr ? PrStringView{text, std::char_traits<char>::length(text)} : PrStringView{};
        push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .key = k, .code = code});
        if (typed.size != 0)
            push(PrAppEvent{.kind = PrAppEventKind_Text, .modifiers = modifiers, .text = typed});
        push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .key = k, .code = code});
        frame();
    }

    void type(const char* text, PrAppModifierFlags modifiers = 0) {
        for (const char* c = text; *c != 0; ++c) {
            push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .key = PrAppKey(*c)});
            push(PrAppEvent{.kind = PrAppEventKind_Text, .modifiers = modifiers, .text = PrStringView{c, 1}});
            push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .key = PrAppKey(*c)});
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
        return prLayoutNodeFirstChild(tree, prLayoutNodeParent(tree, view.transcript));
    }
    std::vector<PrLayoutNode> tabs() const {
        std::vector<PrLayoutNode> out;
        for (PrLayoutNode n = prLayoutNodeFirstChild(tree, strip()); n != kPrLayoutNodeNone;
                n = prLayoutNodeNextSibling(tree, n))
            out.push_back(n);
        return out;
    }
    PrVec2 centre(PrLayoutNode node) const {
        PrRect b = bounds(node);
        return PrVec2{(b.left + b.right) / 2, (b.top + b.bottom) / 2};
    }

    void pointer(PrAppEventKind kind, PrVec2 at) {
        push(PrAppEvent{.kind = kind, .position = at});
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
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .key = 'K', .code = PrAppKeyCode_KeyK});
    h.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .text = PrStringView{kana, 3}});
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .key = 'K', .code = PrAppKeyCode_KeyK});
    h.frame();
    // macOS sends the KeyDown first, then the input method's commit: CompositionEnd, then the text.
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .key = PrAppKey_Enter, .code = PrAppKeyCode_Enter});
    h.push(PrAppEvent{.kind = PrAppEventKind_CompositionEnd, .text = PrStringView{kana, 3}});
    h.push(PrAppEvent{.kind = PrAppEventKind_Text, .text = PrStringView{kana, 3}});
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .key = PrAppKey_Enter, .code = PrAppKeyCode_Enter});
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
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .key = 'K', .code = PrAppKeyCode_KeyK});
    h.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .text = PrStringView{kana, 3}});
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .key = 'K', .code = PrAppKeyCode_KeyK});
    h.frame();
    // macOS sends the KeyDown first, then the input method's cancel: CompositionEnd with no text.
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .key = PrAppKey_Escape, .code = PrAppKeyCode_Escape});
    h.push(PrAppEvent{.kind = PrAppEventKind_CompositionEnd, .text = PrStringView{}});
    h.push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .key = PrAppKey_Escape, .code = PrAppKeyCode_Escape});
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
    h.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = middle});
    h.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .scrollUnit = PrAppScrollUnit_Pixel, .position = middle,
        .delta = PrVec2{0, -400}});
    h.frame();
    h.frame();
    const float scrolled = prLayoutUnitToPx(prLayoutNodeScroll(h.tree, h.view.transcript).y);
    CHECK(scrolled < -100 || scrolled > 100);
    h.commandDigit(2);
    h.type("for f2");
    h.dragTab(0, 3);                         // f1 to the end
    CHECK(h.order() == "f2 f3 f4 f1");
    CHECK(h.active().name == "f2" && h.active().draft == "for f2");
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(h.tree, h.view.transcript).y) == 0);
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
    h.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = middle});
    h.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .scrollUnit = PrAppScrollUnit_Pixel, .position = middle,
        .delta = PrVec2{0, -400}});
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

} // namespace

int main() {
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
    overlayKeepsAllButTheBackgroundOpaque();
    longUnbrokenTextWraps();
    noFriendsShowsTheNotice();
    appendsFollowAtTheBottomAndHoldAViewScrolledUp(0);
    appendsFollowAtTheBottomAndHoldAViewScrolledUp(3);
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window input tests passed\n");
    return 0;
}
