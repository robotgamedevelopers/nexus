// See mock_link.h.

#include "mock_link.h"

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "command.h"
#include "link/bootstrap.h"
#include "sync/sessions.h"

namespace nexus::mock {

bool Client::connect(const std::string& serverCommand, const std::string& clientId, std::string* why) {
    link::Bootstrap b;
    b.host = "localhost";
    if (!link::serverCommandWords(serverCommand, &b.command, why))
        return false;
    b.client = clientId;
    b.next = 1;
    // What ssh would run on the server's machine, run here: the bootstrap's last argument is that line.
    const CommandRunner local = [](const Command& c, int timeoutSeconds) {
        Command here{"/bin/sh", {"-c", c.arguments.back()}, c.arguments.back()};
        return runCommand(here, timeoutSeconds);
    };
    link::Connect connect;
    if (!link::bootstrap(b, local, &connect, why))
        return false;
    try {
        session = std::make_unique<link::ClientSession>(connect.key, "127.0.0.1", connect.port, 1, connect.format);
    } catch (const std::exception& e) {
        *why = std::string("the link could not start: ") + e.what();
        return false;
    }
    return true;
}

bool Client::step(std::string* why) {
    // A failed tick or read is mosh's to recover from, as it is for a window (a datagram refused while the server
    // starts, say): it is kept in `why` for the stall's message, and the round goes on.
    link::freezeClock();
    session->tick(why);
    std::vector<pollfd> fds;
    for (int fd : session->fds())
        fds.push_back(pollfd{fd, POLLIN, 0});
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::min(20, std::max(0, session->waitMs())));
    link::freezeClock();
    for (const pollfd& p : fds) {
        if ((p.revents & POLLIN) != 0)
            session->receive(why);
    }
    if (session->serverEnded()) {
        *why = "the server ended the link session";
        return false;
    }
    return true;
}

bool Client::until(const std::function<bool()>& done, int stallSeconds, std::string* why) {
    using Clock = std::chrono::steady_clock;
    auto progress = [this] {
        return std::hash<std::string>{}(session->state().init_diff()) * 2 + (session->allAcknowledged() ? 1 : 0);
    };
    std::size_t last = progress();
    auto moved = Clock::now();
    while (!done()) {
        if (!step(why))
            return false;
        const std::size_t now = progress();
        if (now != last) {
            last = now;
            moved = Clock::now();
        } else if (Clock::now() - moved > std::chrono::seconds(stallSeconds)) {
            *why = "nothing changed on the server for " + std::to_string(stallSeconds) + " s" +
                (why->empty() ? std::string() : " (the link last said: " + *why + ")");
            return false;
        }
    }
    return true;
}

void Client::close() {
    if (!session)
        return;
    session->shutdown();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string why;
    while (std::chrono::steady_clock::now() < end && !session->shutdownDone()) {
        if (!step(&why))
            break;
    }
    session.reset();
}

} // namespace nexus::mock
