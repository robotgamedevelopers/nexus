// The window, declared each round as a Prax layout tree over lib/ui: the tab strip, the transcript and the prompt. Prax
// lays it out, paints it, routes input to it, and edits the prompt's text (pr/ui_text_edit.h). The view keeps only what
// the model does not: where each tab's transcript is scrolled, and whether it follows the newest entry.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/layout_tree.h"
#include "pr/text_font.h"
#include "pr/ui.h"

#include "model.h"

namespace nexus {

// The fonts and icons the window draws with, made once. Its address owns the window's records (pr/ui_records.h).
struct Look {
    PrTextFont* body = nullptr;
    PrTextFont* tab = nullptr;
    PrTextFont* label = nullptr;
    PrTextFont* meta = nullptr;
    PrDrawList* plus = nullptr;
    PrDrawList* stop = nullptr;
    PrDrawList* mic = nullptr;
    PrDrawList* ring = nullptr;
    bool overlay = false;       // the window background and tab strip are see-through (style::kOverlayOpacity)
};

// Makes the look's fonts in the system UI family of `fonts`, which must outlive the look, and draws its icons.
void openLook(Look& look, PrFontSet* fonts);
void closeLook(Look& look);

// A tab's transcript scroll, kept while another tab shows.
struct TabView {
    float scroll = 0;           // px from the top, while not following
    bool follow = true;         // pinned to the newest entry
    std::size_t seen = 0;       // entries when last shown, so a new one brings the newest back into view
};

struct View {
    Model* model = nullptr;
    const Look* look = nullptr;
    std::string notice;                 // shown in place of the transcript when there are no tabs
    std::vector<TabView> tabs;          // one per model tab
    std::size_t shown = ~std::size_t(0);        // the tab the transcript showed last round
    std::size_t prompted = ~std::size_t(0);     // the tab whose draft the prompt edited last round
};

// Declares the whole window for one round under `parent`, or as the root if `parent` is kPrLayoutNodeNone, and applies
// the round's input: tab clicks, the prompt's edits, and its Enter, Shift-Enter and Command-Enter.
void buildWindow(View& view, PrUi* ui, PrLayoutNode parent);

} // namespace nexus
