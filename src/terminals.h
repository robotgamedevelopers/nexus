// The terminal tabs' shells over Prax's terminal (pr/terminal.h, prax::studio_terminal), as Prax's terminal app
// (apps/terminal) runs them: one process launcher for the app, one PrTerminal per terminal tab keyed by the tab's id,
// prTerminalFrame for the one that shows (its view fills the room the strip leaves in the window) and
// prTerminalService for the others, prTerminalDestroy on close and at exit.
//
// The screen's background is the window's (style::kBackground), see-through at style::kOverlayOpacity in the overlay
// and opaque otherwise, as behind the friends' tabs. Prax takes the background from the kit theme's bg1, opaque
// (terminal.cpp updateTheme 738-751, color8Of 368-371): each frame sets the screen's background colour back to the
// window's, and paints it on the window element (PrTerminalBackground_Parent). A request for Prax: a host-set
// background role that survives palette changes, or a screen opacity apart from the ink.

#pragma once

#include <cstddef>
#include <map>
#include <string>

#include "pr/app.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/process.h"
#include "pr/terminal.h"

#include "view.h"

namespace nexus {

class PraxTerminals final : public Terminals {
public:
    // `app` may be null (no wake, clipboard or notifications), and `window`, the window that holds the terminals, null
    // (no attention requests). `overlay`: the background is see-through.
    PraxTerminals(PrKit* kit, PrFontSet* fonts, PrApp* app, PrAppWindow* window, bool overlay);
    ~PraxTerminals() override;   // destroys every terminal, then the launcher
    PraxTerminals(const PraxTerminals&) = delete;
    PraxTerminals& operator=(const PraxTerminals&) = delete;

    // What a new terminal runs, and where: empty runs the person's login shell in their home directory.
    std::string program;
    std::string directory;

    bool open(std::size_t id) override;
    void close(std::size_t id) override;
    void show(std::size_t id, PrLayoutNode parent) override;
    void update(std::size_t id) override;
    bool ended(std::size_t id) override;
    std::string title(std::size_t id) override;
    void setTextScale(float k) override;

    void closeAll();
    // The text of the terminal's scrollback and screen, a line feed after each line; empty for no such terminal.
    std::string text(std::size_t id);

private:
    PrTerminal* find(std::size_t id) const;
    bool applyBackground(PrTerminal* terminal) const;
    PrTextFontDesc font() const;

    PrKit* kit_;
    PrFontSet* fonts_;
    PrApp* app_;
    PrAppWindow* window_;
    PrColor8 background_;
    float scale_ = 1;
    PrProcessLauncher* launcher_ = nullptr;
    std::map<std::size_t, PrTerminal*> terminals_;
};

} // namespace nexus
