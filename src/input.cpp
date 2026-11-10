#include "input.h"

namespace nexus {

namespace {

constexpr PrAppModifierFlags kNoCaps = PrAppModifierFlags(~PrAppModifierFlag_CapsLock);

} // namespace

Filtered filterEvent(InputState& state, PrAppEventKind kind, PrAppKey, PrAppKeyCode code,
        PrAppModifierFlags modifiers) {
    Filtered out;
    out.modifiers = PrAppModifierFlags(modifiers & kNoCaps);
    switch (kind) {
        case PrAppEventKind_KeyDown:
            // A Text event comes between its key's KeyDown and the next KeyDown, so a new key ends any drop.
            state.dropText = false;
            if (out.modifiers == PrAppModifierFlag_Alt && code >= PrAppKeyCode_Digit1 && code <= PrAppKeyCode_Digit9) {
                out.action = Action::SelectTab;
                out.tab = std::size_t(code - PrAppKeyCode_Digit1);
                out.consume = true;
                state.dropText = true;      // Option-digit also types a character (Option-1 types an inverted !)
            }
            break;
        case PrAppEventKind_Text:
            if (state.dropText) {
                state.dropText = false;
                out.consume = true;
            }
            break;
        default:
            break;
    }
    return out;
}

} // namespace nexus
