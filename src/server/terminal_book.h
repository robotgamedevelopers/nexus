// What the server's terminals are, apart from their processes (docs/terminals.md): which exist, which run and which
// ended, and what each terminal action does to them. Pure logic: no OS, no time, no Prax, so that the decisions are
// unit-tested and the model's trace check drives this very code (models/ServerTerminals.tla, tests/model
// terminals_walk.cpp). server/terminal_host.h runs the processes, the pseudoterminals and the screens, and does only
// what an outcome here says.
//
// The lifecycle (ServerTerminals.tla): a create makes a terminal, Running; its shell's end makes it Ended, with one
// line saying how (never silently: TerminalView::ended); a close removes it, ending its shell if it runs. Input goes
// only to a Running terminal, and a resize to a Running or Ended one. A link that comes or goes changes nothing here:
// a terminal no window shows keeps running. A server that starts again starts a book of a new epoch, which holds none
// of the old run's terminals and never mints their ids (the epoch is in an id's high 32 bits), so input a window sends
// again after a restart reaches no terminal of the new run.
//
// Only the server's owner gets a shell: an action from a link that is not the owner's is refused whole (`owner` in
// apply; server.cpp sets it from the control socket's peer credentials when the link was made).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "sync/actions.h"
#include "sync/terminal_state.h"

namespace nexus::server {

using sync::TerminalId;

// What the host is to do for an action the book took.
struct TerminalEffect {
    enum class Kind { None, Spawn, Write, Resize, Kill };
    Kind kind = Kind::None;
    TerminalId id = 0;
    std::string directory;          // Spawn: where its shell starts (empty: the home directory)
    std::uint32_t columns = 0;      // Spawn and Resize: its screen
    std::uint32_t rows = 0;
};

struct TerminalOutcome {
    std::string refused;            // empty: taken
    TerminalEffect effect;
};

// The screen a create gets when it names none (0 columns or rows).
inline constexpr std::uint32_t kDefaultTerminalColumns = 80;
inline constexpr std::uint32_t kDefaultTerminalRows = 24;

class TerminalBook {
public:
    enum class State { Running, Ended };
    struct Entry {
        State state = State::Running;
        std::string opener;
        std::string directory;
        std::uint32_t columns = 0;
        std::uint32_t rows = 0;
        std::string ended;          // with Ended: how its shell ended, one line, never empty
    };

    // `epoch`: this run of the server's, nonzero (newEpoch in terminal_host.h).
    explicit TerminalBook(std::uint32_t epoch);

    std::uint32_t epoch() const { return epoch_; }

    // Decides one terminal action (sync/actions.h isTerminal) from a link, `owner` when it is the server's owner's.
    TerminalOutcome apply(const sync::Action& action, bool owner);

    // The host's reports. A shell that ended, or one that could not start (`how` says why), makes its terminal Ended;
    // nothing for a terminal the book no longer holds (closed meanwhile).
    void ended(TerminalId id, std::string how);

    const std::map<TerminalId, Entry>& terminals() const { return terminals_; }
    const Entry* find(TerminalId id) const;

private:
    std::uint32_t epoch_;
    std::uint32_t next_ = 1;
    std::map<TerminalId, Entry> terminals_;
};

// The line a terminal's end shows, from a wait status as waitpid gives it: "exited with code N" or "ended by signal N".
std::string endedLine(bool signaled, int code);

} // namespace nexus::server
