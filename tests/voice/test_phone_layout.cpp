// The iPhone's AI page as laid out (src/view.cpp phoneBack, buildWindow), through the window's offscreen frame
// (src/offscreen.h): the AI's title centred on the window's width whatever the back chevron's, a long one shortened
// with an ellipsis to the room both sides of the chevron leave, and the prompt's rounded box inside the room the
// window has (the screen less its safe area and the keyboard, keyboard_room.h contentBox), and the prompt's focus and
// the keyboard still over frames with no input. Pure: made-up time, no window.

#include <cmath>
#include <cstdio>
#include <string>

#include "pr/font_set.h"
#include "pr/layout_query.h"
#include "pr/ui.h"
#include "pr/text_font.h"

#include "pr/app_input.h"

#include "keyboard_room.h"
#include "model.h"
#include "offscreen.h"
#include "view.h"

namespace {

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

constexpr float kWidth = 402, kHeight = 874;  // an iPhone 17 Pro, in points

PrRect boundsOf(nexus::OffscreenFrame& off, PrLayoutNode node) {
    return prLayoutRectToPx(prLayoutNodeBounds(off.kit()->tree, node, PrLayoutCoordinates_Viewport));
}

struct Page {
    nexus::Model model;
    nexus::Look look;
    nexus::View view;
    explicit Page(const std::string& name) : model({{name, ""}}, false) {
        view.model = &model;
        view.look = &look;
        view.idiom = "phone";
        view.phoneMenu = false;
        view.windowApp.panels = {nexus::Panel::Back, nexus::Panel::Transcript, nexus::Panel::Prompt};
        view.windowApp.surface.showTabs = false;
        view.windowApp.surface.tabTitles = {name};
        model.select_tab(0);
    }
};

void theTitleIsCentredOnTheWindow(PrFontSet* fonts, const std::string& name, bool shortened) {
    Page page(name);
    nexus::OffscreenFrame off(page.view, fonts, PrVec2{kWidth, kHeight}, 3, false);
    for (int i = 0; i < 6; ++i)
        off.frame();
    CHECK(page.view.phoneTitle != kPrLayoutNodeNone);
    CHECK(page.view.phoneBackButton != kPrLayoutNodeNone);
    const PrRect title = boundsOf(off, page.view.phoneTitle);
    const PrRect back = boundsOf(off, page.view.phoneBackButton);
    const float centre = (title.left + title.right) / 2;
    const PrRect room = boundsOf(off, page.view.phoneTitleRoom);
    if (std::fabs((title.left + title.right) / 2 - kWidth / 2) > 1 || title.left < back.right)
        std::fprintf(stderr, "  %s -> '%s': title %.1f..%.1f, back %.1f..%.1f, room %.1f..%.1f\n", name.c_str(),
                 page.view.phoneTitleShown.c_str(), title.left, title.right, back.left, back.right, room.left, room.right);
    if (std::fabs(centre - kWidth / 2) > 1)
        if (std::fabs((title.left + title.right) / 2 - kWidth / 2) > 1 || title.left < back.right)
        std::fprintf(stderr, "  %s: the title's centre is %.1f, the window's %.1f\n", name.c_str(), centre, kWidth / 2);
    CHECK(std::fabs(centre - kWidth / 2) <= 1);
    CHECK(title.left >= back.right);  // the chevron never covers the title
    CHECK(title.right <= kWidth - (back.right));  // the same room on the other side
    const bool ellipsis = page.view.phoneTitleShown.size() >= 3 &&
        page.view.phoneTitleShown.compare(page.view.phoneTitleShown.size() - 3, 3, "…") == 0;
    CHECK(ellipsis == shortened);
    if (!shortened) CHECK(page.view.phoneTitleShown == name);
}

// The iPhone's safe area, upright: the status bar and the home indicator.
constexpr PrAppInsets kSafe{62, 0, 34, 0};

// The box the window is built in for a keyboard whose top is at keyboardTop (0: none), as main_ios.cpp works it out.
PrVec2 roomFor(float keyboardTop) {
    const PrVec2 extent{kWidth, kHeight};
    const PrRect shown = keyboardTop > 0 ? PrRect{0, keyboardTop, kWidth, kHeight} : PrRect{};
    return nexus::contentBox(extent, kSafe, nexus::roomAboveKeyboard(extent, kSafe, shown));
}

// The AI's page laid out in the box the window has (FrameHost::buildAbove gives the window that box): the prompt's
// rounded box ends at its foot, never under the home indicator or the keyboard.
void thePromptIsInTheRoomLeft(PrFontSet* fonts, float keyboardTop) {
    Page page("Johnny Grok");
    const PrVec2 box = roomFor(keyboardTop);
    nexus::OffscreenFrame off(page.view, fonts, box, 3, false);
    for (int i = 0; i < 6; ++i)
        off.frame();
    CHECK(page.view.promptBox != kPrLayoutNodeNone);
    const PrRect prompt = boundsOf(off, page.view.promptBox);
    if (prompt.bottom > box.y + 0.5f || prompt.bottom < box.y - 60)
        std::fprintf(stderr, "  the prompt's box ends at %.1f, the room at %.1f\n", prompt.bottom, box.y);
    CHECK(prompt.bottom <= box.y + 0.5f);  // never under the home indicator or the keyboard
    CHECK(prompt.bottom >= box.y - 60);    // and at the foot of the room, not floating up the page
}

// The prompt's microphone is a button inside the rounded box: a press on it is its own, never moved into the text as a
// press on the box's margin is (routePromptPointer), or dictation could never start.
void theMicrophoneTakesItsPress(PrFontSet* fonts) {
    Page page("Johnny");
    nexus::VoicePanel voice;
    int presses = 0;
    voice.press = [&] { ++presses; };
    page.view.voice = &voice;
    page.view.windowApp.surface.voiceMic = true;
    nexus::OffscreenFrame off(page.view, fonts, PrVec2{kWidth, kHeight}, 3, false);
    for (int i = 0; i < 4; ++i)
        off.frame();
    CHECK(page.view.promptMic != kPrLayoutNodeNone);
    const PrRect mic = boundsOf(off, page.view.promptMic);
    const PrRect box = boundsOf(off, page.view.promptBox);
    CHECK(mic.left >= box.left && mic.right <= box.right && mic.top >= box.top && mic.bottom <= box.bottom);
    PrAppEvent down{};
    down.kind = PrAppEventKind_PointerDown;
    down.pointer.button = 0;
    down.pointer.position = {(mic.left + mic.right) / 2, (mic.top + mic.bottom) / 2};
    CHECK(!nexus::routePromptPointer(page.view, off.ui(), down));
    // A press on the box's own margin, left of the text, still goes into the text.
    down.pointer.position = {box.left + 2, (box.top + box.bottom) / 2};
    CHECK(nexus::routePromptPointer(page.view, off.ui(), down));
    (void)presses;
}

// The keyboard over many frames, as UIKit shows it: up while the last frame reported text being edited
// (PrAppOutput.hasTextInput), and down for a frame now and then whatever the window does (its input view changing over,
// a frame whose caret is not yet placed). With the prompt tapped once, the prompt keeps the focus and the keyboard comes
// up once: neither changes again on its own, frame after frame (the flicker it once had). The window's box above the
// keyboard is tests/window/test_keyboard_frames.cpp's, on the frame code main_ios.cpp runs.
void theKeyboardHoldsStill(PrFontSet* fonts) {
    Page page("Johnny");
    nexus::VoicePanel voice;
    voice.press = [] {};
    page.view.voice = &voice;
    page.view.windowApp.surface.voiceMic = true;
    page.view.windowApp.surface.focusPrompt = false;
    nexus::OffscreenFrame off(page.view, fonts, roomFor(0), 3, false);
    for (int i = 0; i < 3; ++i)
        off.frame();
    CHECK(page.view.prompt != kPrLayoutNodeNone);
    prUiSetFocus(off.ui(), page.view.prompt);  // the person's tap
    int focusChanges = 0, keyboardChanges = 0;
    bool focused = true, editing = false;
    for (int i = 0; i < 120; ++i) {
        off.frame();
        const bool nowFocused = prUiFocus(off.ui()) == page.view.prompt && page.view.prompt != kPrLayoutNodeNone;
        const bool nowEditing = prUiOutput(off.ui()).hasTextInput;
        focusChanges += nowFocused != focused;
        keyboardChanges += nowEditing != editing;
        focused = nowFocused;
        editing = nowEditing;
    }
    if (focusChanges > 0 || keyboardChanges > 1)
        std::fprintf(stderr, "  over 120 frames the focus changed %d times and the text input %d\n", focusChanges,
                     keyboardChanges);
    CHECK(focusChanges == 0);
    CHECK(keyboardChanges <= 1);  // up once, after the tap
    CHECK(editing && focused);
}

} // namespace

int main() {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(fonts, system);
    prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
    theTitleIsCentredOnTheWindow(fonts, "Johnny Grok", false);
    theTitleIsCentredOnTheWindow(fonts, "Jo", false);
    theTitleIsCentredOnTheWindow(fonts, "Maximiliana Wolkenstein-Hohenberg of the Northern Reach", true);
    theKeyboardHoldsStill(fonts);
    theMicrophoneTakesItsPress(fonts);
    thePromptIsInTheRoomLeft(fonts, 0);
    thePromptIsInTheRoomLeft(fonts, 538);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (failures) {
        std::fprintf(stderr, "%d phone-layout check(s) failed\n", failures);
        return 1;
    }
    std::printf("phone_layout: all checks passed\n");
    return 0;
}
