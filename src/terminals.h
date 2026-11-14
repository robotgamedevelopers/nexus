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
//
// Nothing here waits on the frame thread (AGENTS.md, the frame budget). A thread of the terminals' own starts each
// shell (prTerminalCreate waits for the fork and exec, and the directory is checked there too) and ends it
// (prTerminalDestroy kills the shell, joins its reader and waits for it), and four times a second reads each shell's
// foreground process group for its tab's label (sysctl, foreground.h), reading the arguments of a new one. The label
// must see the foreground as of each new title (remote.h TabLabel: a title ssh's remote side sets is the session's),
// so a frame whose title changed reads the group itself, one small system call, and the arguments only if the group
// is new: once per command, never once a frame. A tab opened shows at once, empty, and its shell
// appears in it once started; a shell that cannot start closes its tab. A terminal is used by one thread at a time
// (pr/terminal.h): the start thread hands each one it made to the frame thread, and the frame thread hands back each
// one to end, using it no more. What must be asked of a terminal on its own thread, the directory its shell works in
// (prTerminalWorkingDirectory, a process call), is read on the frame thread once a second, one terminal a frame. A
// new text scale is applied to a terminal when it shows, not to every terminal at once.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pr/app.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/process.h"
#include "pr/terminal.h"

#include "remote.h"
#include "view.h"

namespace nexus {

class PraxTerminals final : public Terminals {
public:
    // `app` may be null (no wake, clipboard or notifications), and `window`, the window that holds the terminals, null
    // (no attention requests). `overlay`: the background is see-through.
    PraxTerminals(PrKit* kit, PrFontSet* fonts, PrApp* app, PrAppWindow* window, bool overlay);
    ~PraxTerminals() override;   // ends every terminal and waits for them (at exit), then the launcher
    PraxTerminals(const PraxTerminals&) = delete;
    PraxTerminals& operator=(const PraxTerminals&) = delete;

    // What a new terminal runs, and where: empty runs the person's login shell in their home directory.
    std::string program;
    std::string directory;
    // The fleet's machines, which name a terminal's tab while its shell runs ssh to one of them (remote.h).
    std::vector<Machine> machines;

    bool open(std::size_t id) override;         // in `directory`
    // Starts the shell of the tab `id` in `dir`, empty for the home directory, on the start thread. A `dir` that is no
    // longer a directory starts it in the home directory, with one line on stderr saying so. False, having printed why,
    // if no shell can start here (no launcher); a shell that fails to start prints why and ends (ended).
    bool openIn(std::size_t id, const std::string& dir);
    void close(std::size_t id) override;        // hands its terminal to the start thread to end
    void show(std::size_t id, PrLayoutNode parent) override;
    void update(std::size_t id) override;
    bool ended(std::size_t id) override;
    // The tab's label (remote.h TabLabel): while ssh runs in the foreground, the fleet machine it reaches or its host;
    // otherwise the title the shell set, or the one from before a session that ended.
    std::string title(std::size_t id) override;
    // The title the program set, as Prax tells it (prTerminalTitle), whatever the label; empty for no such terminal.
    std::string programTitle(std::size_t id);
    void setTextScale(float k) override;

    void closeAll();
    // Reads each terminal's foreground process and title, once a frame at the frame's `time` in seconds, for its label.
    void observe(double time);
    // The text of the terminal's scrollback and screen, a line feed after each line; empty for no such terminal.
    std::string text(std::size_t id);
    // The directory the terminal's shell works in, as Prax tells it (prTerminalWorkingDirectory: the shell's own report,
    // else its foreground process's directory, else where it started), as last read (observe); for a shell not started
    // yet, the directory it was asked to start in. Empty if it names none, or for no such terminal.
    std::string workingDirectory(std::size_t id);
    // Reads every terminal's directory now, as the window closes and its terminals are saved.
    void readDirectories();

private:
    // A shell to start, as the frame thread asked for it.
    struct Start {
        std::size_t id;
        std::string program;
        std::string dir;
        float scale;
    };
    // A terminal's foreground process group and the remote label of what it runs (remote.h), as read at `at`.
    struct Seen {
        int group = -1;
        std::string remote;
        std::chrono::steady_clock::time_point at{};
    };
    // A shell the start thread started (`terminal` null: it could not), with its process id (-1: not found).
    struct Started {
        std::size_t id;
        PrTerminal* terminal;
        int shell;
        float scale;
        std::string dir;
    };

    PrTerminal* find(std::size_t id);
    bool applyBackground(PrTerminal* terminal) const;
    PrTextFontDesc font(float scale) const;
    void take();                                // the frame thread takes the terminals started since
    void run();                                 // the start thread
    Started start(const Start& s);              // on the start thread
    void readDirectory(std::size_t id, PrTerminal* terminal);

    PrKit* kit_;
    PrFontSet* fonts_;
    PrApp* app_;
    PrAppWindow* window_;
    PrColor8 background_;
    float scale_ = 1;
    PrProcessLauncher* launcher_ = nullptr;     // used on the start thread only

    // The frame thread's.
    std::map<std::size_t, PrTerminal*> terminals_;
    std::map<std::size_t, TabLabel> labels_;
    std::map<std::size_t, int> pids_;           // each terminal's shell, by process id; absent if not found
    std::map<std::size_t, Seen> seen_;          // each terminal's foreground as the label last saw it
    std::map<std::size_t, std::string> titles_; // each terminal's title at the label's last observation
    std::map<std::size_t, std::string> asked_;  // shells asked for and not yet taken, with the directory asked for
    std::set<std::size_t> failed_;              // shells that could not start, until their tab closes
    std::map<std::size_t, std::string> dirs_;   // each terminal's directory as last read
    std::map<std::size_t, float> scales_;       // the text scale each terminal's font is at
    double dirsAt_ = 0;                         // when the last round of reading directories started, s
    std::size_t dirNext_ = 0;                   // the next terminal (by id) whose directory is read

    // Shared with the start thread, under `mutex_`.
    std::mutex mutex_;
    std::condition_variable wake_;
    bool ending_ = false;
    std::deque<Start> starts_;
    std::vector<PrTerminal*> ends_;
    std::vector<Started> started_;
    std::map<std::size_t, int> shells_;         // each terminal's shell, by process id, for the start thread to read
    std::vector<Machine> watched_;              // `machines`, as the frame thread last handed them over
    std::map<std::size_t, Seen> watch_;         // each terminal's foreground as the start thread last saw it change
    std::thread thread_;                        // last: it starts once everything above is made
};

} // namespace nexus
