#include "server/secret_lines.h"

#include "driver/protocol.h"

namespace nexus::server {

void SecretLines::answered(const sync::SecretReply& reply) {
    if (reply.asker.session == 0) {
        return;
    }
    const driver::Encoded line = driver::encode(driver::SecretAnswer{reply.asker.ask, reply.stored, reply.reason});
    if (!line.error.empty()) {
        if (toLog_) {
            toLog_("nexus-server: session " + std::to_string(reply.asker.session) + ": the answer to secret request " +
                   std::to_string(reply.id) + " was not written: " + line.error);
        }
        return;
    }
    if (toDriver_) {
        toDriver_(reply.asker.session, line.line);
    }
}

void SecretLines::logged(const sync::SecretLog& line) {
    if (toLog_) {
        toLog_("nexus-server: " + line.line());
    }
}

} // namespace nexus::server
