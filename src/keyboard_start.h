// How the on-screen keyboard rises on iOS and iPadOS (main_ios.cpp laidOut). Prax's UIKit view stays the first responder
// while nothing is edited, with an empty input view that keeps the keyboard down, and when a frame starts editing text
// it swaps that view for the keyboard in place (uikit_view.mm setEditing, reloadInputViews). The system then slides
// the keyboard's panel up empty and its keys come in after, as no other app's keyboard does. A view that is not the
// first responder is made it instead (becomeFirstResponder), and the system presents the keyboard whole, keys and all,
// as it does for a text field. So the window lets the view go as the first responder in the frame that starts editing,
// before Prax raises the keyboard at the frame's end. Pure, for the unit test (tests/ios/keyboard_unit.cpp).

#pragma once

namespace nexus {

// What the end of a frame does about the keyboard.
enum class KeyboardStart {
    Keep,       // nothing: the frame edits text as the last did, or edits none
    Present,    // the frame starts editing text: let the view go as the first responder, so Prax presents the keyboard
};

// Whether the frames edit text, as Prax's view takes it from each frame's output (PrAppOutput::hasTextInput).
struct TextInputStart {
    bool editing = false;

    // The end of a frame whose output edits text if `hasTextInput`.
    KeyboardStart frameEnds(bool hasTextInput) {
        const bool starts = hasTextInput && !editing;
        editing = hasTextInput;
        return starts ? KeyboardStart::Present : KeyboardStart::Keep;
    }
};

} // namespace nexus
