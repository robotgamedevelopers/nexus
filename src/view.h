// Draws the window from the model into a Prax draw list, in logical pixels. Immediate: every frame records the whole
// window again. Keeps only what drawing needs between frames (scroll per tab, where the tabs were for clicks).

#pragma once

#include <string>
#include <vector>

#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/text.h"

#include "model.h"

namespace nexus {

struct Fonts {
    const PrFont* regular = nullptr;
    const PrFont* semibold = nullptr;
    const PrFont* symbols = nullptr;   // fallback for code points the regular font lacks
    PrTextShaper* shaper = nullptr;
    PrTextGlyphBuffer* buffer = nullptr;
};

// Opens the system fonts. Returns false with `error` set if a font does not load.
bool openFonts(Fonts& fonts, std::string& error);
void closeFonts(Fonts& fonts);

struct ViewState {
    std::vector<float> scroll;        // per tab: px scrolled up from the bottom of the transcript
    std::vector<PrRect> tabRects;     // where the last frame drew each tab
    float maxScroll = 0;              // of the active tab, as of the last frame
    bool caretOn = true;
};

void drawWindow(PrDrawList* list, const Model& model, ViewState& view, const Fonts& fonts, PrVec2 extent);

// Returns the tab under `point`, or -1.
int tabAt(const ViewState& view, PrVec2 point);

// Scrolls the active tab's transcript by `dy` px (positive scrolls toward older entries).
void scrollBy(const Model& model, ViewState& view, float dy);

} // namespace nexus
