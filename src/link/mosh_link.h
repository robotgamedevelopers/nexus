// The link between the window and nexus-server: mosh's Network::Transport (mosh 1.4.0, cmake/mosh.cmake) carrying
// nexus's two states, the person's actions up (sync/actions.h) and the sessions down (sync/sessions.h). mosh does the
// rest: UDP, AES-128-OCB with the session's key, roaming, heartbeats, pacing, fragmentation and resending by diffs.
//
// These classes keep mosh's types out of nexus's headers. mosh reads a clock it freezes (util/timestamp.h): call
// freezeClock() before each round of work. mosh's state is per process and not thread-safe (its clock, its
// compressor's buffer): one thread per process uses these classes.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sync/actions.h"
#include "sync/sessions.h"

namespace nexus::link {

void freezeClock();

// nexus-server's end of one client's session.
class ServerSession {
public:
    // Binds a UDP port (mosh's range, 60001 to 60999, every interface) and makes the session's key. Throws
    // std::runtime_error, saying why, when no port can be bound. `applied` is the client's applied counter before this
    // session (the store's), and `next` the number the client's first action in this session has (its ClientSession's
    // `next`): a window reconnecting sends again, under their own numbers, the actions it has no acknowledgement for,
    // and those at or below `applied` are skipped.
    ServerSession(const sync::Sessions& state, std::uint64_t applied = 0, std::uint64_t next = 1);
    ~ServerSession();
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    std::string port() const;
    std::string key() const;    // for the one CONNECT line only: never logged or written anywhere
    std::vector<int> fds() const;

    // Reads one datagram; call when an fd is readable. False, with why, for one mosh refused or could not read.
    bool receive(std::string* why);
    // The actions this client sent that are new: each once, in number order (mosh's get_remote_diff, then the
    // session's Applier). `problem` says why, when a diff could not be read or was refused.
    std::vector<sync::Action> take(std::string* problem);
    void setState(const sync::Sessions& state);
    // Sends what is due. False, with why, when mosh could not.
    bool tick(std::string* why);
    int waitMs();
    bool ended() const;         // the client shut the session down, and its shutdown was acknowledged
    std::uint64_t applied() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The window's end.
class ClientSession {
public:
    // `ip` is numeric. Throws std::runtime_error, saying why, for a bad key, address or port. `next` is the number the
    // first action added gets (the server's session must be made with the same).
    ClientSession(const std::string& key, const std::string& ip, const std::string& port, std::uint64_t next = 1);
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    std::vector<int> fds() const;
    bool receive(std::string* why);
    std::uint64_t add(sync::Action action);    // its num ignored: the next number
    const sync::Sessions& state() const;        // the server's latest
    std::uint64_t stateNum() const;         // its number: 0 until the server is first heard
    std::uint64_t heardMsAgo() const;       // since the server's latest state arrived
    bool allAcknowledged() const;           // the server has acknowledged every action added
    bool tick(std::string* why);
    int waitMs();
    void shutdown();                        // starts mosh's shutdown; tick until shutdownDone()
    bool shutdownDone() const;              // acknowledged, or given up on

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nexus::link
