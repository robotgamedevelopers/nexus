#include "sync/formats.h"

namespace nexus::sync {

std::string windowRefusal(std::uint32_t window, std::uint32_t oldest) {
    if (window >= oldest) {
        return std::string();
    }
    return "This window is too old for nexus-server (it reads state format " + std::to_string(window) +
        ", and the server sends " + std::to_string(oldest) + " or later): update nexus.";
}

std::string serverRefusal(std::uint32_t server, std::uint32_t oldest) {
    if (server >= oldest) {
        return std::string();
    }
    return "nexus-server is too old for this window (it sends state format " + std::to_string(server) +
        ", and the window reads " + std::to_string(oldest) + " or later): update nexus-server, then run "
        "`nexus-server stop` so the next window starts the new one.";
}

} // namespace nexus::sync
