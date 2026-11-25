// Unit tests of the on-screen keyboard's parts that need no device. The room above it (src/keyboard_room.h): the
// content, which starts at the safe area's top, ends at the keyboard's top, with neither inset counted again. An iPad's
// frame (1376 by 1032 points, 24 at the top and 20 at the foot) is the case that sat the content's foot under the
// keyboard by both insets. And how it rises (src/keyboard_start.h): the calls a frame's end makes to the view, in order,
// against a view that does as Prax's UIKit view does, so that the keyboard is presented whole and never swapped in for
// the empty input view. Pure: no window, no clock.

#include <cstdio>
#include <string>
#include <vector>

#include "keyboard_room.h"
#include "keyboard_start.h"

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

constexpr PrVec2 kIPad{1376, 1032};
constexpr PrAppInsets kIPadSafe{24, 0, 20, 0};

void theContentEndsAtTheKeyboardsTop() {
    // A docked keyboard whose top is at 640: the content runs from 24 to 640, and its foot is the keyboard's top.
    const PrRect keyboard{0, 640, 1376, 1032};
    const float room = roomAboveKeyboard(kIPad, kIPadSafe, keyboard);
    CHECK(room == 640 - 24);
    CHECK(kIPadSafe.top + room == keyboard.top);
    // An iPhone's: 402 by 874, 62 at the top and 34 at the foot, a keyboard from 538.
    CHECK(roomAboveKeyboard({402, 874}, {62, 0, 34, 0}, {0, 538, 402, 874}) == 538 - 62);
}

void noKeyboardOrNoneOverTheContentLeavesItWhole() {
    CHECK(roomAboveKeyboard(kIPad, kIPadSafe, PrRect{}) == 0);
    // A hardware keyboard's bar under the safe area's foot covers none of the content.
    CHECK(roomAboveKeyboard(kIPad, kIPadSafe, {0, 1012, 1376, 1032}) == 0);
    CHECK(roomAboveKeyboard(kIPad, kIPadSafe, {0, 1020, 1376, 1032}) == 0);
    // A keyboard over the whole content leaves no room, never less.
    CHECK(roomAboveKeyboard(kIPad, kIPadSafe, {0, 10, 1376, 1032}) == 0);
}

// The view as Prax's UIKit view is (uikit_view.mm): the first responder from the time it shows, with the empty input
// view while it edits nothing; setEditing, at each frame's end, swaps the input view in place if it is the first
// responder (reloadInputViews) and else becomes it. Each call is logged.
struct PraxView {
    bool firstResponder = false;
    bool editing = false;
    std::vector<std::string> calls;

    void shown() {
        firstResponder = true;
        calls.push_back("become empty");
    }
    void resign() {
        if (!firstResponder) return;
        firstResponder = false;
        calls.push_back("resign");
    }
    void setEditing(bool e) {
        if (e == editing) return;
        editing = e;
        if (firstResponder) {
            calls.push_back(e ? "reload keyboard" : "reload empty");
        } else {
            firstResponder = true;
            calls.push_back(e ? "become keyboard" : "become empty");
        }
    }
};

// One frame's end as main_ios.cpp laidOut and Prax's deliverFrame make it: the window's say, then Prax's setEditing.
void frameEnds(TextInputStart& start, PraxView& view, bool hasTextInput) {
    if (start.frameEnds(hasTextInput) == KeyboardStart::Present) view.resign();
    view.setEditing(hasTextInput);
}

using Calls = std::vector<std::string>;

void theKeyboardIsPresentedWholeEachTimeItRises() {
    TextInputStart start;
    PraxView view;
    view.shown();                       // the window shows: the view is the first responder, with no keyboard
    frameEnds(start, view, false);      // the Dashboard: no text
    CHECK((view.calls == Calls{"become empty"}));
    frameEnds(start, view, true);       // an AI's tab: its prompt takes the focus
    CHECK((view.calls == Calls{"become empty", "resign", "become keyboard"}));
    frameEnds(start, view, true);       // and keeps it: nothing more
    frameEnds(start, view, true);
    CHECK(view.calls.size() == 3);
    frameEnds(start, view, false);      // back to the Dashboard: the keyboard goes
    CHECK(view.calls.back() == "reload empty");
    frameEnds(start, view, true);       // and the prompt again: presented whole again
    CHECK((Calls(view.calls.end() - 2, view.calls.end()) == Calls{"resign", "become keyboard"}));
    for (const std::string& call : view.calls) CHECK(call != "reload keyboard");
}

void theFirstFrameEditingTextPresentsTheKeyboard() {
    // A launch straight onto an AI's tab: the first frame edits text.
    TextInputStart start;
    PraxView view;
    view.shown();
    frameEnds(start, view, true);
    CHECK((view.calls == Calls{"become empty", "resign", "become keyboard"}));
    CHECK(view.firstResponder && view.editing);
}

} // namespace

int main() {
    theContentEndsAtTheKeyboardsTop();
    noKeyboardOrNoneOverTheContentLeavesItWhole();
    theKeyboardIsPresentedWholeEachTimeItRises();
    theFirstFrameEditingTextPresentsTheKeyboard();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("keyboard unit tests passed\n");
    return 0;
}
