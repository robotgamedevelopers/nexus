// The window's input, headless: the nexus view built over a Prax UI context and Studio Kit with no window and no GPU,
// fed events as the window would feed them (through takeEvent), with the model checked after each. Frames carry a
// made-up clock (from 1000 s), never the real one. Text is set in the system's fonts.

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

    Harness() : Harness(std::vector<FriendInfo>{FriendInfo{"alex", "m1"}, FriendInfo{"emma", "m2"}}) {}

    explicit Harness(const std::vector<FriendInfo>& friends) : model(friends) {
        prFontSetSetSystem(fonts, system);
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
        PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        PrKitTheme theme = windowTheme(fonts, false);
        kit = prKitCreate(ui, &theme);
        openLook(look);
        view.model = &model;
        view.look = &look;
        frame();
        frame();
    }

    ~Harness() {
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        closeLook(look);
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
    CHECK(h.active().transcript.size() == 1);
    CHECK(h.active().transcript.size() == 1 && h.active().transcript[0].kind == EntryKind::Queued);
    CHECK(h.active().transcript.size() == 1 && h.active().transcript[0].text == "later");
    CHECK(h.active().draft.empty());
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
    CHECK(h.active().transcript.size() == 2 && h.active().transcript[1].kind == EntryKind::Queued);
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

void appendsFollowAtTheBottomAndHoldAViewScrolledUp() {
    Harness h;
    for (int i = 0; i < 30; ++i)
        h.model.append(0, Entry{EntryKind::Friend, "line " + std::to_string(i)});
    h.frame();
    PrRect port = h.bounds(h.view.transcript);
    std::vector<PrLayoutNode> e = h.entries();
    CHECK(e.size() == 30);
    // At the bottom: the newest entry shows, at the foot of the transcript.
    CHECK(!e.empty() && h.bounds(e.back()).bottom <= port.bottom + 0.5f);
    CHECK(!e.empty() && h.bounds(e.back()).bottom > port.bottom - 2 * style::kU);
    h.model.append(0, Entry{EntryKind::Friend, "newest"});
    h.frame();
    e = h.entries();
    CHECK(e.size() == 31 && h.bounds(e.back()).bottom <= port.bottom + 0.5f);
    CHECK(e.size() == 31 && h.bounds(e.back()).bottom > port.bottom - 2 * style::kU);

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
    appendsFollowAtTheBottomAndHoldAViewScrolledUp();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window input tests passed\n");
    return 0;
}
