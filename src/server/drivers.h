// nexus-server's friends: one driver supervisor (server/supervisor.h) per friend tab, and the processes it asks for.
// This is the hub's Responder: a prompt sent to a tab goes to the tab's supervisor, whose entries and durable state are
// committed with the window's action, and only then is anything written to a driver. It runs inside the server's one
// poll loop: each driver is a child process with non-blocking pipes for its standard input and output (its standard
// error goes to the server's log), in a process group of its own so that a signal reaches the harness it runs too. Its
// output is read a bounded chunk per round and cut into protocol lines, and its exit is collected without waiting. A
// driver that floods is read no faster than its effects are committed, so it fills its pipe and waits, and the link to
// the windows keeps its turn in the loop.
//
// Which program is a tab's driver comes from the drivers file (server/driver_table.h), read at each prompt.
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
    };

    // Over the tabs in `store`; attach() the hub that commits to it before anything else.
    Drivers(const sync::TabStore& store, Options options);
    ~Drivers() override;            // ends every driver still running: input closed, SIGKILL, collected
    Drivers(const Drivers&) = delete;
    Drivers& operator=(const Drivers&) = delete;

    void attach(sync::Hub& hub) { hub_ = &hub; }

    // What the server finds when it starts: each tab whose driver had a turn open has it cut, and each with prompts
    // waiting starts a driver for them. False when the store did not take a step.
    bool start(Millis now);

    // sync::Responder.
    std::vector<sync::TabChange> send(const std::string& tab, const std::string& prompt) override;
    void sent(const std::string& tab, bool committed) override;

    // Stops the friend's work in `tab` (the person's stop; on the wire in a later part). False when the store did not
    // take a step.
    bool stop(const std::string& tab, Millis now);
    // The server is stopping: every driver is ended. Serve on until alive() is false.
    bool shutdown(Millis now);
    bool alive() const;     // any driver process not yet collected

    // The loop: the descriptors to poll and how long the poll may wait; after it, serve() writes and reads what it
    // can, collects exits and passes deadlines. False when the store did not take a step; *changed when a tab changed.
    std::vector<pollfd> fds() const;
    int waitMs(Millis now) const;
    bool serve(Millis now, bool* changed);

    const Supervisor* supervisor(const std::string& tab) const;   // for the tests
    pid_t pid(const std::string& tab) const;                      // -1 when no driver runs

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
    struct Tab {
        explicit Tab(Supervisor s) : sup(std::move(s)) {}
        Supervisor sup;
        std::unique_ptr<Process> proc;
    };

    ResolvedDriver resolve(const std::string& tab) const;
    Config config(const std::string& tab, const ResolvedDriver& driver) const;
    bool run(const std::string& tab, Output out, Millis now, bool committed);
    bool commit(const std::string& tab, const Output& out, bool* changed = nullptr);
    void carryOut(const std::string& tab, Tab& t, const Command& c, std::vector<Output>& more, Millis now);
    void flushInput(Process& p, Millis now);
    void readSome(Tab& t, Output& out, Millis now, std::size_t limit);
    void collect(const std::string& tab, Tab& t, Output& out, Millis now);

    const sync::TabStore& store_;
    Options options_;
    sync::Hub* hub_ = nullptr;
    std::map<std::string, Tab> tabs_;
    // A send between send() and sent().
    std::optional<Supervisor> pendingSup_;
    Output pendingOut_;
};

} // namespace nexus::server
