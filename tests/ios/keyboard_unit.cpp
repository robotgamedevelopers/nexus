// Unit tests of the room above the on-screen keyboard (src/keyboard_room.h): the content, which starts at the safe
// area's top, ends at the keyboard's top, with neither inset counted again. An iPad's frame (1376 by 1032 points, 24 at
// the top and 20 at the foot) is the case that sat the content's foot under the keyboard by both insets. Pure: no
// window, no clock.

#include <cstdio>

#include "keyboard_room.h"

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

} // namespace

int main() {
    theContentEndsAtTheKeyboardsTop();
    noKeyboardOrNoneOverTheContentLeavesItWhole();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("keyboard unit tests passed\n");
    return 0;
}
