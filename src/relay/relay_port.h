// The window's link on Windows (link_port.h LinkPort): nexus-link-relay inside WSL2 (relay/relay_wire.h), started with
// wsl.exe, spoken to over its standard input and output. Every decision is relay/relay_life.h's (RelayLife, bound to
// models/RelayLife.tla by its trace): this is the processes, the pipes and the threads around it.
//
// Three threads of its own per relay, none of them the window's: one writes what RelayLife hands it, one reads the
// relay's frames and wakes the window, one copies the relay's standard error to the window's (its log). Neither poll
// nor send waits on the relay.
//
// When the relay ends (it crashed, WSL2 stopped, or its bytes were not the protocol), the port waits for its process to
// exit before it tells RelayLife, so that two relays never run at once. The state's phase is then Failed and its
// problem says why (its exit code, the last line of its standard error, and what its process wrote before the
// relay's Hello, as plain text: wsl.exe writes its own errors there, in UTF-16), with the sessions the window had; the
// actions the relay took with it unacknowledged are in the state's cut, for the window to show; the next relay starts
// after RelayLife's backoff (1 s, doubling, at most 10 s).

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "link_port.h"
#include "relay/relay_life.h"
#include "relay/relay_words.h"

namespace nexus::relay {

class RelayPort final : public LinkPort {
public:
    struct Options {
        std::string host = "localhost";
        std::vector<std::string> relay;         // relayCommand's `relay` (relay/relay_words.h)
        std::string wsl = "wsl.exe";            // found on the PATH (System32)
        std::function<void()> wake;             // called on the port's threads when poll has something new
    };

    explicit RelayPort(Options options);
    ~RelayPort() override;      // closes the relay's input (it ends the link: mosh's shutdown), waits up to 3 s
    RelayPort(const RelayPort&) = delete;
    RelayPort& operator=(const RelayPort&) = delete;

    bool poll(LinkState* out) override;
    void requestHistory(sync::SessionId id, std::uint64_t epoch, std::uint64_t before) override;
    std::uint64_t send(sync::Action action) override;

private:
    struct Process;
    void run();

    Options options_;
    std::mutex mutex_;
    std::condition_variable changed_;
    RelayLife life_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace nexus::relay
