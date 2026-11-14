// Where the secret dialog's effects go in nexus-server (docs/secrets.md): each asker's one answer, as the `secret` line
// its driver reads (docs/driver-protocol.md), and each log line, to server.log. Neither ever holds a value
// (sync/secrets.h). The two places are functions, so the server sends them to its drivers and its standard error, and
// the trace walk and the tests record the exact bytes each receives.

#pragma once

#include <functional>
#include <string>

#include "sync/hub.h"

namespace nexus::server {

class SecretLines : public sync::SecretEvents {
public:
    // A line for session `session`'s driver, which the caller writes to it if it still waits (Drivers::tellSecret).
    using ToDriver = std::function<void(sync::SessionId session, const std::string& line)>;
    // A line for server.log, with its "nexus-server: " and without its newline.
    using ToLog = std::function<void(const std::string& line)>;

    SecretLines(ToDriver toDriver, ToLog toLog) : toDriver_(std::move(toDriver)), toLog_(std::move(toLog)) {}

    // The answer, as its driver line, to the asker's driver; nothing for nexus itself (session 0), which asked no
    // driver. An answer that cannot be written as a line is logged, without it.
    void answered(const sync::SecretReply& reply) override;
    void logged(const sync::SecretLog& line) override;

private:
    ToDriver toDriver_;
    ToLog toLog_;
};

} // namespace nexus::server
