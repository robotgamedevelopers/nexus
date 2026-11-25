// nexus-server's terminals (docs/terminals.md): each a login shell on a pseudoterminal of the server's own, started and
// owned by the server, so that a window that cannot start a process (iOS) has terminal tabs, and so that a terminal
// keeps running while no window is linked: a window that links again finds it as it left it.
//
// The host does what the book decides (server/terminal_book.h, models/ServerTerminals.tla): it starts a shell for a
// create, writes a window's input to its shell, resizes its pseudoterminal, ends it on a close. It reads every shell's
// output into a terminal emulator of its own (Prax's lib/vterm), keeps the input modes the program set
// (terminal_screen.h), and gives the screens to the sessions state (views) as rows of painted text, each row rebuilt
// only when lib/vterm says it changed. A shell that ends is reaped and its terminal shows how it ended, in its last
// row's line and in TerminalView::ended, until a window closes it.
//
// The shell is the one the Mac window's terminal tab runs (terminal_launch.h, Prax's PrTerminalDesc with an empty
// program): the person's login shell (SHELL, else the password database's, else /bin/sh) with -l, in the directory
// asked for when it is one, else the home directory, with TERM=xterm-256color and COLORTERM=truecolor, and the
// server's environment less every NEXUS_ variable (the server's own settings, which no shell needs).
//
// What it keeps: the screens and the modes, in memory only. Nothing a terminal shows or is sent is written to the
// store, a log or a file; a refusal's line names the terminal by number and never holds its input.
//
// One thread, the server's: no call waits. Output is read without blocking, at most kReadBudget bytes per terminal per
// serve, so that a shell that writes without end costs each pass a bounded time; input the pseudoterminal cannot take
// now waits in order behind what it holds, up to kPendingLimit bytes, past which the terminal's input is refused.

#pragma once

#include <poll.h>
#include <sys/types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "server/terminal_book.h"
#include "server/terminal_screen.h"
#include "sync/terminal_state.h"

struct PrVterm;

namespace nexus::server {

// What a terminal's shell is: the program and its arguments, its environment, and the home directory it falls back to.
struct ShellLaunch {
    std::string program;
    std::vector<std::string> arguments;     // after argv[0], which is `program`
    std::vector<std::string> environment;   // NAME=value each
    std::string home;
};

// The login shell from `environ` and the password database, as the header says.
ShellLaunch loginShell(char** environ);

// A new epoch for a run of the server: nonzero, from the system's random source.
std::uint32_t newEpoch();

class TerminalHost {
public:
    static constexpr std::size_t kReadBudget = 64 * 1024;
    static constexpr std::size_t kPendingLimit = 1024 * 1024;

    TerminalHost(std::uint32_t epoch, ShellLaunch launch);
    ~TerminalHost();    // ends every shell: SIGHUP to its process group, then SIGKILL to what is left, and reaps them
    TerminalHost(const TerminalHost&) = delete;
    TerminalHost& operator=(const TerminalHost&) = delete;

    std::uint32_t epoch() const { return book_.epoch(); }
    const TerminalBook& book() const { return book_; }

    // One terminal action from a link (`owner`: made by the server's owner). Returns the refusal, empty when taken.
    std::string apply(const sync::Action& action, bool owner);

    // The pseudoterminals to poll: each for input, and for output while input waits for it.
    std::vector<pollfd> fds() const;
    // How long the server's poll may wait for the terminals alone: 50 ms while a shell's output ended and its exit is
    // not yet reaped, -1 otherwise.
    int waitMs() const;
    // Reads what each shell wrote, writes what waits for each, reaps each that ended. True when a screen changed.
    bool serve();

    // The terminals as the sessions state carries them, in id order.
    std::vector<sync::TerminalRef> views();

private:
    struct Running;
    void spawn(const TerminalEffect& e);
    void write(Running& t, const std::string& bytes);
    void flush(Running& t);
    void end(Running& t, std::string how);
    void kill(Running& t);
    sync::TerminalRef view(TerminalId id, Running& t);

    TerminalBook book_;
    ShellLaunch launch_;
    std::map<TerminalId, std::unique_ptr<Running>> running_;
    std::vector<pid_t> closing_;         // shells of closed terminals, signalled and not yet reaped
    bool changed_ = true;
};

} // namespace nexus::server
