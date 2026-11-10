#include "overlay.h"

namespace nexus {

// Each show and each hide starts the overlay's keyboard history again.
OverlayAct OverlayRules::act(OverlayAct a) {
    if (a != OverlayAct::None)
        keyed_ = false;
    return a;
}

OverlayAct OverlayRules::hotkey(bool visible, bool active) {
    return act(visible && active ? OverlayAct::Hide : OverlayAct::Show);
}

OverlayAct OverlayRules::escape(bool visible) {
    return act(visible ? OverlayAct::Hide : OverlayAct::None);
}

OverlayAct OverlayRules::observe(bool visible, bool active) {
    if (!visible) {
        keyed_ = false;
        return OverlayAct::None;
    }
    if (active) {
        keyed_ = true;
        return OverlayAct::None;
    }
    return act(keyed_ ? OverlayAct::Hide : OverlayAct::None);
}

OverlayAct OverlayRules::keyCheck(bool visible, bool active) {
    if (visible && active) {
        keyed_ = true;
        return OverlayAct::None;
    }
    return act(visible ? OverlayAct::Hide : OverlayAct::None);
}

bool overlayCloseFailed(bool opened, bool stopping) {
    return !opened && !stopping;
}

} // namespace nexus
