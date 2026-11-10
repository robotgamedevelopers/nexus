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

// A window with no window: the view, its model of two friends, and a kit over a UI context over a layout tree.
struct Harness {
    Model model{{FriendInfo{"alex", "m1"}, FriendInfo{"emma", "m2"}}};
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    Look look;
    View view;
    double time = 1000;

    Harness() {
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
        if (!takeEvent(view, ui, e))
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

    // Option-digit as macOS sends it: key Unknown (the layout's character), the digit's code, and the character typed.
    void optionDigit(int n) {
        key(PrAppKey_Unknown, PrAppKeyCode(PrAppKeyCode_Digit1 + n - 1), PrAppModifierFlag_Alt, "\xC2\xA1");
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

void optionDigitSelectsATabAndTypesNothing() {
    Harness h;
    h.optionDigit(2);
    CHECK(h.model.active() == 1);
    h.optionDigit(1);
    CHECK(h.model.active() == 0);
    h.optionDigit(9);       // no tab 9
    CHECK(h.model.active() == 0);
    CHECK(h.model.tab(0).draft.empty());
    CHECK(h.model.tab(1).draft.empty());
}

void capsLockKeepsTheBindings() {
    Harness h;
    h.type("hi", PrAppModifierFlag_CapsLock);
    h.enter(PrAppModifierFlag_CapsLock);
    CHECK(h.active().transcript.size() == 1 && h.active().transcript[0].text == "hi");
    h.type("x", PrAppModifierFlag_CapsLock);
    h.enter(PrAppModifierFlags(PrAppModifierFlag_Super | PrAppModifierFlag_CapsLock));
    CHECK(h.active().transcript.size() == 2 && h.active().transcript[1].kind == EntryKind::Queued);
    h.optionDigit(2);
    h.key(PrAppKey_Unknown, PrAppKeyCode_Digit1, PrAppModifierFlags(PrAppModifierFlag_Alt | PrAppModifierFlag_CapsLock));
    CHECK(h.model.active() == 0);
}

void eachTabKeepsItsDraft() {
    Harness h;
    h.type("one");
    h.optionDigit(2);
    h.type("two");
    h.optionDigit(1);
    CHECK(h.model.tab(0).draft == "one");
    CHECK(h.model.tab(1).draft == "two");
    h.type("!");            // the caret comes back at the draft's end
    CHECK(h.model.tab(0).draft == "one!");
    h.optionDigit(2);
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
    optionDigitSelectsATabAndTypesNothing();
    capsLockKeepsTheBindings();
    eachTabKeepsItsDraft();
    enterWhileComposingCommitsOnly();
    appendsFollowAtTheBottomAndHoldAViewScrolledUp();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window input tests passed\n");
    return 0;
}
