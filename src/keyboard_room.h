// The room the window's content has above the on-screen keyboard (iOS and iPadOS, main_ios.cpp build). Prax's Studio
// window pads its panel to the safe area and gives the application the frame's whole extent, so the content starts at
// the safe area's top; while the keyboard covers part of the foot, the content is the box from there to the keyboard's
// top. Pure, for the unit test (tests/ios/keyboard_unit.cpp).

#pragma once

#include <algorithm>

#include "pr/app_window.h"

namespace nexus {

// The height, in the frame's logical pixels, of the content above the keyboard: from the safe area's top to the
// keyboard's top, never below zero. Zero when the keyboard covers none of the content (none shows, or it stands at or
// below the safe area's foot, as a hardware keyboard's bar may), and the content keeps its height.
inline float roomAboveKeyboard(PrVec2 extent, const PrAppInsets& safe, const PrRect& keyboard) {
    if (keyboard.bottom <= keyboard.top || keyboard.right <= keyboard.left) return 0;
    const float foot = extent.y - safe.bottom;
    if (keyboard.top >= foot) return 0;
    return std::max(0.0f, keyboard.top - safe.top);
}

} // namespace nexus
