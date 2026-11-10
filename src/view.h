// The window, declared each round as a Prax layout tree over lib/ui and Studio Kit: the tab strip (kit tabs), the
// transcript and the prompt (a kit text area). Prax lays it out, paints it, routes input to it, edits the prompt's text
// and keeps the transcript at its newest entry. The view keeps only what Prax does not: each tab's transcript scroll
// while another tab shows.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/layout_tree.h"
#include "pr/ui.h"

#include "input.h"
#include "model.h"

namespace nexus {

// What the window draws that no kit part does, made once and remade at a new text scale. Its address owns the window's
// records (pr/ui_records.h).
struct Look {
    PrDrawList* ring = nullptr;     // the context ring beside the model label
    bool overlay = false;           // the window background and tab strip are see-through (style::kOverlayOpacity)
    int textSteps = 0;              // the text scale it is drawn at, in steps from 1 (style::textScale)
};

void openLook(Look& look);
void closeLook(Look& look);

// The kit theme the window is drawn in: Studio's dark theme with the window's colours and type sizes (style.h) at the
// text scale `steps` steps from 1.
PrKitTheme windowTheme(PrFontSet* fonts, bool overlay, int steps = 0);

struct View {
    Model* model = nullptr;
    Look* look = nullptr;
    int askedTextSteps = 0;                     // the text scale the keys asked for, applied at the next frame's start
    std::string notice;                         // shown in place of the transcript when there are no tabs
    std::vector<float> scrolls;                 // each tab's transcript scroll, kept while another tab shows
    std::size_t shown = ~std::size_t(0);        // the tab the transcript showed last round
    std::size_t prompted = ~std::size_t(0);     // the tab whose draft the prompt edited last round
    uint32_t promptRows = 1;                    // lines of text the prompt showed at the last layout
    PrLayoutNode prompt = kPrLayoutNodeNone;    // the prompt's text area, as the last round built it
    PrLayoutNode transcript = kPrLayoutNodeNone; // the transcript's scroller, as the last round built it
};

// Called at the start of each frame, before its rounds: applies a text scale the keys asked for in the last frame to the
// look and the kit's theme (which changes only between rounds). Returns true if it changed the scale.
bool beginWindowFrame(View& view, PrKit* kit, PrFontSet* fonts);

// True if a text scale is asked for and not yet applied: another frame would show something different.
bool windowWantsFrame(const View& view);

// Declares the whole window for one round under `parent`, or as the root if `parent` is kPrLayoutNodeNone, and applies
// the round's input: tab clicks, Command-1 to Command-0, the text scale's keys, the prompt's edits, and its Enter and Command-Enter.
void buildWindow(View& view, PrKit* kit, PrLayoutNode parent);

// Runs one event through the event filter (input.h). Returns true if the window took it, having queued the event
// without Caps Lock in its place; false if the caller should queue the event as it is.
bool takeEvent(PrUi* ui, const PrAppEvent& event);

} // namespace nexus
