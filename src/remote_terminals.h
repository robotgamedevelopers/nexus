// Terminal tabs whose shells run on nexus-server (docs/terminals.md): the window of a machine that cannot start a
// process of its own (iOS) shows the server's terminals in its tabs, and types into them. The server owns each shell,
// its pseudoterminal and its screen; the window holds a copy of the screen for each tab, drawn by Prax's terminal view
// (pr/terminal.h) with no program behind it, and painted from the rows the state brings (sync/terminal_paint.h).
//
// What a tab does:
//   - Command-T (or the phone's New terminal row) asks the server for a terminal (TerminalCreate) named by a token of
//     this window's (its opener); the tab shows nothing until the state holds a terminal with that opener, then shows
//     it. A create the server applied that made no terminal (at most kMaxTerminals run) shows why in the tab.
//   - A terminal in the state that no tab of this window holds (made by another window, or by this one before it was
//     closed and opened again) gets a tab of its own at the end of the terminals, the selection staying where it was:
//     a window that links again finds every terminal as it left it.
//   - Keys, text and pastes typed while the tab shows go to the shell as TerminalInput, translated by the screen's own
//     input rules under the modes the program set (TerminalView::modes, which the paint sets on the copy); a release
//     and a pointer are the view's (selection, scrolling). The key row (Escape, Tab, Control, the arrows) sends the
//     same as the keys would.
//   - The view's extent, when it changes, goes to the server as TerminalResize.
//   - A shell that ended stays in its tab with the line that says how (the server writes it on the screen) and takes
//     no input; one lost to the server's restart shows kTerminalRestarted; either stays until the tab is closed. One
//     another window closed leaves this window too (ended() is true, and the view closes its tab).
//   - Command-W (or the close control) sends TerminalClose and the tab goes, as on the Mac.
//
// What it keeps: each screen in memory, and nothing in a file, a log or the store; a line it prints names a tab by
// number, never what it shows or what was typed. Input rides in the action's secret (sync::SecretValue), wiped when
// the server acknowledges it.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "pr/app.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/layout_tree.h"

#include "model.h"
#include "sync/actions.h"
#include "sync/sessions.h"
#include "sync/terminal_state.h"
#include "view.h"

struct PrTerminal;
struct PrVterm;

namespace nexus {

struct LinkPort;

// What a terminal tab's keys send: the bytes a screen's input rules (pr/vterm_input.h) make of each key, text and
// paste, under the modes the program set, in the order they were typed; and the key row's Control, held for the next
// key.
class TerminalKeys {
public:
    // Appends to bytes what e sends (cell: the screen's cell, px), and says whether e was the terminal's. On iOS a
    // hardware key's Text event comes from the text system after its KeyDown, perhaps after the next keys: a key that
    // types text is sent at its KeyDown, and its own Text dropped when it comes (within a second, by the events' time).
    bool type(PrVterm* vterm, PrVec2 cell, const PrAppEvent& e, std::string* bytes);
    // Appends to bytes what the key row's key sends, with Control if it was held.
    void key(PrVterm* vterm, PrVec2 cell, PrAppKey key, PrAppModifierFlags modifiers, std::string* bytes);
    void toggleControl() { control_ = !control_; }
    void dropControl() { control_ = false; }
    bool controlHeld() const { return control_; }

private:
    struct SentText {
        std::string text;
        double time = 0;
    };
    bool control_ = false;
    std::deque<SentText> sent_;     // the text of each key sent at its KeyDown, oldest first, until its Text comes
};

class RemoteTerminals : public Terminals {
public:
    // `token`: this window's, random per launch, that names the terminals it asks for (TerminalView::opener).
    RemoteTerminals(PrApp* app, PrFontSet* fonts, std::string token);
    ~RemoteTerminals() override;
    RemoteTerminals(const RemoteTerminals&) = delete;
    RemoteTerminals& operator=(const RemoteTerminals&) = delete;

    void setLink(LinkPort* link) { link_ = link; }
    // The kit of the window's tree, while the window is open (each frame's start).
    void setKit(PrKit* kit) { kit_ = kit; }

    // The state the link brought (`connected`: while the server is heard): binds each waiting tab to its terminal,
    // adds a tab for each terminal no tab holds, and keeps each screen for its next paint.
    void received(const sync::Sessions& state, bool connected, Model& model) override;

    // Whether a new terminal can be asked for now: the server runs them, is heard, and runs fewer than its most.
    bool available() const override;

    // An event while the tab `id` shows: a key, text or paste goes to its shell (true, taken); the rest is the view's.
    bool typed(std::size_t id, const PrAppEvent& e) override;
    // The key row's keys: the bytes the screen's input rules make of `key` with `modifiers`, sent to the tab's shell.
    void key(std::size_t id, PrAppKey key, PrAppModifierFlags modifiers);
    // The key row's Control: the next key or letter typed is sent with Control.
    void toggleControl() { keys_.toggleControl(); }
    bool controlHeld() const { return keys_.controlHeld(); }

    bool open(std::size_t id) override;
    void close(std::size_t id) override;
    void show(std::size_t id, PrLayoutNode parent) override;
    void update(std::size_t id) override;
    bool ended(std::size_t id) override;
    std::string problem(std::size_t id) override;
    std::string title(std::size_t id) override;
    void setTextScale(float k) override;

private:
    struct Tab {
        std::string opener;                 // the token this window named it by; empty for one adopted
        std::uint64_t createAction = 0;     // the number of its TerminalCreate, while it waits for its terminal
        sync::TerminalId terminal = 0;      // 0 while it waits
        std::uint64_t epoch = 0;            // the server's epoch when the state first held it
        sync::TerminalRef now;              // the screen as the state last brought it
        sync::TerminalRef held;             // the screen as last painted on the copy
        PrTerminal* view = nullptr;         // the copy, made on the tab's first show
        bool launched = false;              // the copy's first frame is past (it wrote its own line, which is cleared)
        std::uint32_t columns = 0, rows = 0;            // the copy's extent at the last paint
        std::uint32_t sentColumns = 0, sentRows = 0;    // the last extent sent to the server
        sync::TerminalFate fate = sync::TerminalFate::Running;
        bool closedElsewhere = false;       // the server no longer holds it, in its run: another window closed it
        std::string refused;                // the create the server applied made no terminal: why
    };

    Tab* find(std::size_t id);
    void paint(Tab& t);
    void send(sync::Action action);
    void sendBytes(Tab& t, std::string bytes);
    PrTextFontDesc font() const;

    PrApp* app_;
    PrFontSet* fonts_;
    std::string token_;
    LinkPort* link_ = nullptr;
    PrKit* kit_ = nullptr;
    float scale_ = 1;
    TerminalKeys keys_;
    bool served_ = false, connected_ = false;
    std::size_t count_ = 0;                 // the terminals the state holds
    std::uint64_t epoch_ = 0;
    std::map<std::size_t, Tab> tabs_;
    std::map<std::size_t, float> scales_;
    std::set<sync::TerminalId> closing_;    // closed by this window, until the state no longer holds them
    std::set<std::string> orphans_;         // openers of tabs closed while they waited: closed when they come
};

} // namespace nexus
