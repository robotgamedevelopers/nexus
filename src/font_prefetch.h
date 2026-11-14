// Loads the system's fallback fonts before a frame needs them. The first emoji, CJK or Arabic text the window sets makes
// the font set find and load a fallback face; with nothing loaded that took 184 ms on the frame that first showed such
// text. The window starts this at launch, while nothing shows: on a thread of its own it sets a sample of each script
// the window is likely to meet, in each of the window's faces, in a layout of its own over the window's font set, so
// that the set finds and loads the faces there (a font set is safe on any thread: pr/font_set.h).
//
// What it cannot do: each text font of the window's kit still matches the set's fallback faces itself the first time a
// cluster needs one (Prax text_font.cpp matchFallback), on the frame thread, and a kit makes its own text fonts
// (kit.cpp makeFonts). That part, about 100 ms, stays on the frame that first shows such text until Prax can match a
// text font's fallbacks ahead of need or off the frame thread (docs/frame-budget.md, known hitches).

#pragma once

#include <thread>

#include "pr/font_set.h"
#include "pr/text.h"

namespace nexus {

// Makes the faces of the window's text fonts in `shaper` before the window's first frame. A shaper makes a font's face
// (CoreText reads the font's tables) the first time it shapes with a font of that file; on the window's first frame that
// was over half its work. This sets a line of Latin text in every text role of the window's theme, in a layout of its
// own over `shaper` and `fonts`, so that the faces are made and kept in the shaper (faces are shared by every font made
// from a file). Called on the thread that uses `shaper`, before its first frame.
void warmShaper(PrTextShaper* shaper, PrFontSet* fonts, bool overlay);

class FontPrefetch {
public:
    // Starts on a thread of its own; `fonts` must outlive it.
    explicit FontPrefetch(PrFontSet* fonts);
    ~FontPrefetch();     // waits for the thread
    FontPrefetch(const FontPrefetch&) = delete;
    FontPrefetch& operator=(const FontPrefetch&) = delete;

    // Waits until the samples are set.
    void wait();

private:
    std::thread thread_;
};

} // namespace nexus
