// The window's offscreen frame: the same tree the window builds (view.h buildWindow), laid out and painted without a
// window or a GPU, and written to a PNG. `nexus --screenshot` on the desktop (main.cpp) and its iOS counterpart
// (tests/ios/state_shot.cpp) both draw through it, so a frame of a state file is drawn one way everywhere.
//
// A workaround: Prax has no public offscreen render of a UI tree (only its test fixture, tools/harness/kit_fixture.h).

#pragma once

#include <functional>

#include "pr/app_display.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/ui.h"

#include "view.h"

namespace nexus {

class OffscreenFrame {
public:
    // The window at `extent` px for a display of `ratio`. `seeThrough` draws it as the overlay is drawn (style.h);
    // otherwise opaque, since an offscreen frame has nothing behind it to show through.
    OffscreenFrame(View& view, PrFontSet* fonts, PrVec2 extent, float ratio, bool seeThrough);
    ~OffscreenFrame();
    OffscreenFrame(const OffscreenFrame&) = delete;
    OffscreenFrame& operator=(const OffscreenFrame&) = delete;

    PrKit* kit() const { return kit_; }
    PrUi* ui() const { return ui_; }

    // One frame at the next 60 Hz time from 1000 s, made-up time: `before` runs ahead of the tree's rounds (the sprint
    // board's page frame), `after` once they end.
    void frame(const std::function<void(double, const PrAppDisplay&)>& before = {},
        const std::function<void()>& after = {});

    // Renders the last frame on the CPU and writes it to `path` as a PNG, over coloured stripes with `stripes` (in
    // place of the screen behind a see-through overlay). False, having printed why on stderr, if it could not.
    bool write(const char* path, bool stripes);

private:
    View& view_;
    PrVec2 extent_;
    float ratio_;
    PrTextShaper* shaper_ = nullptr;
    PrLayoutTree* tree_ = nullptr;
    PrUi* ui_ = nullptr;
    PrKit* kit_ = nullptr;
    PrAppDisplay display_{};
    int frames_ = 0;
};

} // namespace nexus
