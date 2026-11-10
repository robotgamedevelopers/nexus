#include "input.h"

namespace nexus {

namespace {

constexpr PrAppModifierFlags kNoCaps = PrAppModifierFlags(~PrAppModifierFlag_CapsLock);

constexpr PromptKey kPromptKeys[] = {
    {PrAppKey_Enter, 0, Action::Send},
    {PrAppKey_Enter, PrAppModifierFlag_Shift, Action::Newline},
    {PrAppKey_Enter, PrAppModifierFlag_Super, Action::Queue},
};

} // namespace

Filtered filterEvent(InputState& state, PrAppEventKind kind, PrAppKey key, PrAppKeyCode code,
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
            } else if (key == PrAppKey_Enter && state.composing) {
                out.consume = true;         // the input method commits its text; Enter neither sends nor breaks a line
            }
            break;
        case PrAppEventKind_Text:
            if (state.dropText) {
                state.dropText = false;
                out.consume = true;
            }
            break;
        case PrAppEventKind_CompositionUpdate:
            state.composing = true;
            break;
        case PrAppEventKind_CompositionEnd:
            state.composing = false;
            break;
        default:
            break;
    }
    return out;
}

const PromptKey* promptKeys(std::size_t* count) {
    *count = sizeof kPromptKeys / sizeof kPromptKeys[0];
    return kPromptKeys;
}

Action promptAction(PrAppKey key, PrAppModifierFlags modifiers) {
    PrAppModifierFlags held = PrAppModifierFlags(modifiers & kNoCaps);
    for (const PromptKey& k : kPromptKeys) {
        if (k.key == key && k.modifiers == held)
            return k.action;
    }
    return Action::None;
}

} // namespace nexus
