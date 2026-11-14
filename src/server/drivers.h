// nexus-server's friends: one driver supervisor (server/supervisor.h) per session, and the processes it asks for.
// Several sessions may be with one friend: each has a driver of its own. This is the hub's Responder: a prompt sent in
// a session goes to the session's supervisor, whose entries and durable state are committed with the window's action,
// and only then is anything written to a driver. A prompt queued goes at the end of the session's queue, and the
// supervisor takes the oldest at a turn's end (one per turn; a take and the person's entry are one step of the store).
// A stop goes to the session's supervisor at once, never behind anything, once the hub has counted the action. A
// change of the session's model or effort goes with its next turn's send; a change of its account ends its driver once
// no turn is open, and the next driver starts under the new account (part 5 of the roadmap gives it the account, and a
// summary of the session to start from). The friend's text not yet in a closed entry is the hub's open entry for the
// session (Hub::setOpen), shown to the windows as it grows and kept in memory only: it becomes a durable closed entry
// at its block's end, so a restart loses at most that block, and its turn is cut with a notice. It runs inside the
// server's one poll loop: each driver is a child process with non-blocking pipes for its standard input and output (its
// standard error goes to the server's log), in a process group of its own so that a signal reaches the harness it runs
// too. Its output is read a bounded chunk per round and cut into protocol lines, and its exit is collected without
// waiting. A driver that floods is read no faster than its effects are committed, so it fills its pipe and waits, and
// the link to the windows keeps its turn in the loop.
//
// Which program is a session's driver comes from the drivers file (server/driver_table.h), by the session's friend,
// read at each prompt.
//
// The process must ignore SIGPIPE, as nexus-server does: a driver that exits mid-write is then an error from write.
//
// Prax's process layer (lib/process) is not used: it can only end a process with SIGKILL, so a driver could not be
// sent SIGTERM and given a grace period first, and it serves each process from a thread of its own where the server
// runs one poll loop.

#pragma once

#include <sys/types.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "driver/protocol.h"
#include "server/driver_table.h"
#include "server/supervisor.h"
#include "sync/hub.h"

struct pollfd;

namespace nexus::server {

// How long a driver whose output ended, or that stopped taking its input, has to exit before that is taken as the
// driver closing a pipe while it runs. A judgement, not measured: an exit follows the end of a process's output within
// milliseconds.
constexpr Millis kPipeGraceMs = 1000;

// The longest the loop waits while a driver process is alive, so that an exit is collected even when a process the
// driver started outside its group holds its output open (no end of output wakes the poll). A judgement, not measured:
// nexus-server's own loop wakes at least this often.
constexpr Millis kCollectMs = 1000;

// How often the drivers file is read again for the sessions that show down for want of a driver, so that one given a
// driver shows idle (and its queued prompts go on) without waiting for the person's next prompt. A judgement, not
// measured: the file is small, and a person who has just fixed it looks back within seconds.
constexpr Millis kResolveMs = 1000;

// The most of one driver's output read in one round of the loop. A judgement, not measured: a round's commits stay
// short, while a driver streaming text is read far faster than a person reads it.
constexpr std::size_t kReadChunk = 64 * 1024;

class Drivers : public sync::Responder {
public:
    struct Options {
        std::string table;              // the drivers file's path
        std::string home;               // the home directory, for a friend with no directory
        Timing timing;
        std::function<Millis()> clock;  // now, for the prompts the hub hands over
        std::string machine{};          // this server's machine, by its name in the fleet (server/home.h
                                        // serverMachine): an instance on another machine, or every instance while
                                        // this is empty, starts no driver here yet
        std::string hostName{};         // this machine's full host name, which an instance may name instead
                                        // (server/home.h sameMachine)
    };

    // Over the sessions in `store`; attach() the hub that commits to it before anything else.
    Drivers(const sync::SessionStore& store, Options options);
    ~Drivers() override;            // ends every driver still running: input closed, SIGKILL, collected
    Drivers(const Drivers&) = delete;
    Drivers& operator=(const Drivers&) = delete;

    void attach(sync::Hub& hub) { hub_ = &hub; }

    // What the server finds when it starts: each session whose driver had a turn open has it cut, and each with prompts
    // waiting starts a driver for them. False when the store did not take a step.
    bool start(Millis now);

    // sync::Responder: a send, a queue (Command-Enter) and a stop (Command-Escape) of the person's, each committed by
    // the hub with the window's action before anything is written to a driver.
    std::vector<sync::SessionChange> send(sync::SessionId id, const std::string& prompt) override;
    std::vector<sync::SessionChange> queue(sync::SessionId id, const std::string& prompt) override;
    std::vector<sync::SessionChange> stop(sync::SessionId id) override;
    void sent(sync::SessionId id, bool committed) override;
    // sync::Responder: what a new session with a friend is (its harness; no default account, model or effort yet), and
    // a session's settings, changed: from its next turn on.
    sync::SessionInfo defaults(const std::string& friendName) const override;
    void configured(sync::SessionId id, const sync::SessionInfo& info) override;
    // sync::Responder: a session just made shows down when its friend has no driver configured.
    void made(sync::SessionId id) override;
    // The server is stopping: every driver is ended. Serve on until alive() is false.
    bool shutdown(Millis now);
    bool alive() const;     // any driver process not yet collected

    // The loop: the descriptors to poll and how long the poll may wait; after it, serve() writes and reads what it can,
    // collects exits and passes deadlines. False when the store did not take a step; *changed when a session changed.
    std::vector<pollfd> fds() const;
    int waitMs(Millis now) const;
    bool serve(Millis now, bool* changed);

    const Supervisor* supervisor(sync::SessionId id) const;   // for the tests
    pid_t pid(sync::SessionId id) const;                      // -1 when no driver runs

private:
    struct Process {
        pid_t pid = -1;
        int in = -1;                    // its standard input, our end
        int out = -1;                   // its standard output, our end
        std::string pending;            // bytes not yet written to it
        bool closeInput = false;        // close its input once `pending` is written
        driver::DriverReader reader;
        std::optional<Millis> brokenSince;  // its output ended, or its input stopped taking bytes
        std::string broken;             // which: "output" or "input"
        bool brokenTold = false;        // the supervisor was told
    };
    struct Slot {
        explicit Slot(Supervisor s) : sup(std::move(s)) {}
        Supervisor sup;
        std::unique_ptr<Process> proc;
    };

    std::string friendOf(sync::SessionId id) const;
    ResolvedDriver resolve(sync::SessionId id) const;
    const accounts::Instance* instanceOf(const sync::SessionInfo& info) const;
    ResolvedDriver resolveFriend(const std::string& friendName, DriverTable* table = nullptr) const;
    Config config(sync::SessionId id, const ResolvedDriver& driver) const;
    bool run(sync::SessionId id, Output out, Millis now, bool committed, bool* changed = nullptr);
    Supervisor& pending(sync::SessionId id, const ResolvedDriver& driver, Millis now);
    std::vector<sync::SessionChange> changes(const Output& out, const Supervisor& sup) const;
    bool showOpen(sync::SessionId id, const Slot& t);
    bool showResolved(sync::SessionId id, const ResolvedDriver& driver);
    bool commit(sync::SessionId id, const Output& out, bool* changed = nullptr);
    void carryOut(sync::SessionId id, Slot& t, const Command& c, std::vector<Output>& more, Millis now);
    void flushInput(Process& p, Millis now);
    void readSome(Slot& t, Output& out, Millis now, std::size_t limit);
    void collect(sync::SessionId id, Slot& t, Output& out, Millis now);

    const sync::SessionStore& store_;
    Options options_;
    sync::Hub* hub_ = nullptr;
    std::map<sync::SessionId, Slot> slots_;
    // A send between send() and sent().
    std::optional<Supervisor> pendingSup_;
    Output pendingOut_;
    std::set<sync::SessionId> unresolved_;  // the sessions shown down for want of a driver: read again each kResolveMs
    Millis resolveAt_ = 0;
};

} // namespace nexus::server
