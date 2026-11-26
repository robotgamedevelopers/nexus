// A headless client of the mock over the real link: the mock's seeder (nexus_mock.cpp) and its functional test
// (mock_e2e.cpp) connect through it. It runs the client's own bootstrap (link/bootstrap.h) with the mock's server
// command, on this machine without ssh (the line ssh would run on the server's machine, run by /bin/sh here), then
// talks to the server over mosh's transport on loopback, as any window does.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "link/mosh_link.h"

namespace nexus::mock {

struct Client {
    std::unique_ptr<link::ClientSession> session;

    // Connects as the client `clientId`. False, with why.
    bool connect(const std::string& serverCommand, const std::string& clientId, std::string* why);
    // One round: tick, wait up to 20 ms for the server, read. False, with why, once the server has ended the session.
    bool step(std::string* why);
    // Steps until `done` holds; false, with why, once nothing the server holds or has acknowledged has changed for
    // `stallSeconds` (a slow machine that is still making progress is not cut short), or the link fails.
    bool until(const std::function<bool()>& done, int stallSeconds, std::string* why);
    // Ends the link session as a window that quits does.
    void close();
};

} // namespace nexus::mock
