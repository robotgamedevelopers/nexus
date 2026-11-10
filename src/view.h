// The window, declared each round as a Prax layout tree over lib/ui and Studio Kit: the tab strip (kit tabs), the
// transcript and the prompt (a kit text area). Prax lays it out, paints it, routes input to it, edits the prompt's text
// and keeps the transcript at its newest entry. The view keeps only what Prax does not: each friend's transcript scroll
// while another tab shows.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/layout_tree.h"
#include "pr/ui.h"

#include "input.h"
#include "model.h"

namespace nexus {

// How the window's own records are drawn. Its address owns the window's records (pr/ui_records.h).
struct Look {
    bool overlay = false;           // the window background and tab strip are see-through (style::kOverlayOpacity)
    int textSteps = 0;              // the text scale it is drawn at, in steps from 1 (style::textScale)
};

// The kit theme the window is drawn in: Studio's dark theme with the window's colours and type sizes (style.h) at the
// text scale `steps` steps from 1.
PrKitTheme windowTheme(PrFontSet* fonts, bool overlay, int steps = 0);

// The shells of the terminal tabs, by Tab::id. Our own seam (Prax's terminal cannot run without a process), so that the
// tab logic runs in tests without one; terminals.h is the one over Prax's terminal (pr/terminal.h).
struct Terminals {
    virtual ~Terminals() = default;
    virtual bool open(std::size_t id) = 0;      // starts a new tab's shell; false, having printed why, if it cannot
    virtual void close(std::size_t id) = 0;     // ends its shell and frees it
    // Declares the one that shows this round under `parent`, `extent` px, and gives it focus.
    virtual void show(std::size_t id, PrLayoutNode parent, PrVec2 extent) = 0;
    virtual void update(std::size_t id) = 0;    // services one that does not show this round
    virtual bool ended(std::size_t id) = 0;     // its shell exited and all of its output was read
    virtual std::string title(std::size_t id) = 0;  // the title its shell set, or empty
    virtual void setTextScale(float k) = 0;     // every terminal's font, at the text scale k
};

// The sprint tab's page: nova-sprint's served board. Our own seam, as Terminals is, so that the tab logic runs in tests
// without a page; board.h is the one over Prax's HTML view (pr/html_view.h).
struct Board {
    virtual ~Board() = default;
    // Declares the page under `parent`, `extent` px, if there is one to show, and returns true. Otherwise declares
    // nothing, writes to `problem` one line that says what is happening or what failed and what to do, and returns false.
    virtual bool show(PrLayoutNode parent, PrVec2 extent, std::string* problem) = 0;
    virtual void hidden() = 0;                  // the sprint tab does not show this round
    virtual void setTextScale(float k) = 0;     // the page's zoom, at the text scale k
};

struct View {
    Model* model = nullptr;
    Look* look = nullptr;
    Terminals* terminals = nullptr;             // the terminal tabs' shells; unset, Command-T adds none
    Board* board = nullptr;                     // the sprint tab's page; unset, the tab says it has none
    PrVec2 extent{};                            // the window's extent in px, which the host sets before each frame
    int askedTextSteps = 0;                     // the text scale the keys asked for, applied at the next frame's start
    std::string notice;                         // shown in place of the transcript when there are no tabs
    std::function<void()> hide;                 // Escape: hides the overlay (platform_mac.h hideOverlay); unset when
                                                // windowed, where Escape does nothing
    std::function<void()> stop;                 // Command-Escape: stops the friend's work; nothing to stop yet
    std::function<void()> moved;                // after a drag moved a tab: saves the order (tab_order.h)
    std::vector<float> scrolls;                 // each friend's transcript scroll, by Tab::id, kept while another shows
    std::size_t shown = ~std::size_t(0);        // the friend (Tab::id) the transcript showed last round
    std::size_t prompted = ~std::size_t(0);     // the friend (Tab::id) whose draft the prompt edited last round
    uint32_t promptRows = 1;                    // lines of text the prompt showed at the last layout
    PrLayoutNode prompt = kPrLayoutNodeNone;    // the prompt's text area, as the last round built it
    PrLayoutNode promptBox = kPrLayoutNodeNone; // the rounded box that holds the plus and the text area
    PrLayoutNode transcript = kPrLayoutNodeNone; // the transcript's scroller, as the last round built it
    PrLayoutNode boardBox = kPrLayoutNodeNone;  // the box that holds the sprint tab's page, as the last round built it
};

// Called at the start of each frame, before its rounds: applies a text scale the keys asked for in the last frame to the
// look and the kit's theme (which changes only between rounds). Returns true if it changed the scale.
bool beginWindowFrame(View& view, PrKit* kit, PrFontSet* fonts);

// True if a text scale is asked for and not yet applied: another frame would show something different.
bool windowWantsFrame(const View& view);

// Declares the whole window for one round under `parent`, or as the root if `parent` is kPrLayoutNodeNone, and applies
// the round's input: tab clicks and drags (reordering), Command-1 to Command-0 (by position), the text scale's keys,
// Command-T (a terminal tab) and Command-W (closes a terminal tab), on a friend's or the sprint tab Escape (hide), and
// on a friend's tab Command-Escape (stop), the prompt's edits, and its Enter and Command-Enter. The sprint tab stands
// first in a strip of its own, which nothing is dragged into or out of, and shows its page (or the line that says why
// there is none) with no prompt. A terminal tab shows its terminal in place of the transcript and the prompt; terminals
// that do not show are serviced, and one whose shell ended closes.
void buildWindow(View& view, PrKit* kit, PrLayoutNode parent);

// Runs one event through the event filter (input.h). Returns true if the window took it, having queued the event
// without Caps Lock in its place; false if the caller should queue the event as it is.
bool takeEvent(PrUi* ui, const PrAppEvent& event);

} // namespace nexus
