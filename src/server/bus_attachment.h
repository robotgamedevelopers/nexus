#pragma once

#include <cstddef>
#include <string>

namespace nexus::server {

inline constexpr std::size_t kBusInlineBytes = 4096;
inline constexpr std::size_t kBusAttachmentBytes = 32 * 1024 * 1024; // same limit as nexus-receive

struct BusBody {
    std::string displayBody; // synced to windows as the body of the sender's bubble
    std::string promptBody;  // delivered to the friend (the relative incoming path for a file)
};

// workspace is the receiving session's registered directory on this server, never a path from the bus.
// Successful staging of the same (recipient, messageId) reuses the same durable attachment after a crash.
bool prepareBusBody(const std::string& workspace, const std::string& recipient, const std::string& messageId,
                    const std::string& body, BusBody& result, std::string& why);

} // namespace nexus::server
