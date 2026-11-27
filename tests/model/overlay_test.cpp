// Unit tests of nexus::OverlayRules (src/overlay.h): when the overlay shows and hides. What the window reports comes in
// as arguments, and the window is a stand-in that does what the rules answer. Logic only: no window, no time, no OS.

#include "overlay.h"

#include <cstdio>

namespace {

using nexus::OverlayAct;
using nexus::OverlayRules;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

// The window, carrying out the rules' answers as main.cpp's carryOut does. `grant` is the OS's answer to a show.
struct Overlay {
    OverlayRules rules;
    bool visible = false;
    bool active = false;
    bool grant = true;

    OverlayAct carry(OverlayAct a) {
        if (a == OverlayAct::Show) {
            visible = true;
            active = grant;
        } else if (a == OverlayAct::Hide) {
            visible = false;
            active = false;
        }
        return a;
    }
    OverlayAct hotkey() { return carry(rules.hotkey(visible, active)); }
    OverlayAct escape() { return carry(rules.escape(visible)); }
    OverlayAct frame() { return carry(rules.observe(visible, active)); }
    OverlayAct keyCheck() { return carry(rules.keyCheck(visible, active)); }
};

// Launch shows the overlay as a hotkey press does, and it takes the keyboard at once.
void launchShowsWithTheKeyboard() {
    Overlay o;
    CHECK(o.hotkey() == OverlayAct::Show);
    CHECK(o.visible && o.active);
    CHECK(o.frame() == OverlayAct::None);
    CHECK(o.rules.keyed());
    CHECK(o.keyCheck() == OverlayAct::None);
    CHECK(o.visible);
}

// The hotkey hides an overlay that is up with the keyboard, and the next press shows it again.
void hotkeyToggles() {
    Overlay o;
    o.hotkey();
    o.frame();
    CHECK(o.hotkey() == OverlayAct::Hide);
    CHECK(!o.visible);
    CHECK(!o.rules.keyed());
    CHECK(o.hotkey() == OverlayAct::Show);
    CHECK(o.visible && o.active);
}

// Command-Tab (or a click on another app) takes the keyboard: the next frame hides the overlay, and the next hotkey
// press shows it, not merely re-activates it.
void losingTheKeyboardHides() {
    Overlay o;
    o.hotkey();
    o.frame();
    o.active = false;
    CHECK(o.frame() == OverlayAct::Hide);
    CHECK(!o.visible);
    CHECK(o.hotkey() == OverlayAct::Show);
    CHECK(o.visible && o.active);
}

// Before the frame sees the loss, the hotkey on an overlay without the keyboard shows it (covers the screen again and
// gives it the keyboard) rather than hide it.
void hotkeyOnAnOverlayWithoutTheKeyboardShows() {
    Overlay o;
    o.hotkey();
    o.frame();
    o.active = false;
    CHECK(o.hotkey() == OverlayAct::Show);
    CHECK(o.visible && o.active);
}

// Escape hides a shown overlay; on a hidden one it does nothing.
void escapeHides() {
    Overlay o;
    CHECK(o.escape() == OverlayAct::None);
    o.hotkey();
    o.frame();
    CHECK(o.escape() == OverlayAct::Hide);
    CHECK(!o.visible);
    CHECK(!o.rules.keyed());
    CHECK(o.escape() == OverlayAct::None);
}

// Frames that keep finding the keyboard with the overlay hide nothing: an input method's candidate window or the emoji
// picker, which do not take the keyboard from it, cannot hide it.
void framesWithTheKeyboardHideNothing() {
    Overlay o;
    o.hotkey();
    for (int i = 0; i < 100; ++i)
        CHECK(o.frame() == OverlayAct::None);
    CHECK(o.visible && o.rules.keyed());
}

// A show the OS answers without the keyboard: frames do not hide it before the keyboard could come (it has not had
// it), and the key check does.
void refusedShowIsHiddenByTheKeyCheck() {
    Overlay o;
    o.grant = false;
    CHECK(o.hotkey() == OverlayAct::Show);
    CHECK(o.visible && !o.active);
    CHECK(o.frame() == OverlayAct::None);
    CHECK(o.frame() == OverlayAct::None);
    CHECK(o.visible && !o.rules.keyed());
    CHECK(o.keyCheck() == OverlayAct::Hide);
    CHECK(!o.visible);
}

// The keyboard arriving late (a click on the overlay) before the check: the check keeps it, and a later loss hides it.
void lateKeyboardIsKept() {
    Overlay o;
    o.grant = false;
    o.hotkey();
    o.active = true;
    CHECK(o.keyCheck() == OverlayAct::None);
    CHECK(o.visible && o.rules.keyed());
    o.active = false;
    CHECK(o.frame() == OverlayAct::Hide);
}

// The keyboard lost before any frame saw it: the frame does not hide (the overlay had not been seen with it), and the
// key check does.
void lossBeforeAnyFrameIsCaughtByTheKeyCheck() {
    Overlay o;
    o.hotkey();
    o.active = false;
    CHECK(o.frame() == OverlayAct::None);
    CHECK(o.keyCheck() == OverlayAct::Hide);
    CHECK(!o.visible);
}

// Each show starts the keyboard history again: a show over an overlay that had the keyboard and lost it is not hidden
// by the frame before the keyboard comes back.
void showStartsAgain() {
    Overlay o;
    o.hotkey();
    o.frame();
    o.active = false;
    o.grant = false;
    CHECK(o.hotkey() == OverlayAct::Show);
    CHECK(!o.rules.keyed());
    CHECK(o.frame() == OverlayAct::None);
    CHECK(o.visible);
}

// A frame of a hidden overlay does nothing and forgets the keyboard.
void hiddenFramesDoNothing() {
    OverlayRules r;
    CHECK(r.observe(false, false) == OverlayAct::None);
    CHECK(r.observe(true, true) == OverlayAct::None);
    CHECK(r.keyed());
    CHECK(r.observe(false, false) == OverlayAct::None);
    CHECK(!r.keyed());
    CHECK(r.keyCheck(false, false) == OverlayAct::None);
}

} // namespace

// A close before the window opened, outside shutdown, is Prax refusing the window (its swap chain): a failure. A close
// after open, or at shutdown, is not.
void closeBeforeOpenFails() {
    CHECK(nexus::overlayCloseFailed(false, false));
    CHECK(!nexus::overlayCloseFailed(true, false));
    CHECK(!nexus::overlayCloseFailed(false, true));
    CHECK(!nexus::overlayCloseFailed(true, true));
}

int main() {
    for (int bits = 0; bits < 16; ++bits) {
        CHECK(nexus::overlayMayRestoreFocus(bits & 1, bits & 2, bits & 4, bits & 8) == (bits == 7));
    }
    closeBeforeOpenFails();
    launchShowsWithTheKeyboard();
    hotkeyToggles();
    losingTheKeyboardHides();
    hotkeyOnAnOverlayWithoutTheKeyboardShows();
    escapeHides();
    framesWithTheKeyboardHideNothing();
    refusedShowIsHiddenByTheKeyCheck();
    lateKeyboardIsKept();
    lossBeforeAnyFrameIsCaughtByTheKeyCheck();
    showStartsAgain();
    hiddenFramesDoNothing();
    if (failures != 0) {
        std::fprintf(stderr, "overlay_test: %d failures\n", failures);
        return 1;
    }
    std::printf("overlay_test: all passed\n");
    return 0;
}
