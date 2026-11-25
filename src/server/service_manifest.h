#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::server {

// The validated command of one service owned by this server. argv contains the
// complete invocation, including the seat's nova-secrets wrapper when needed.
struct ManagedService {
    std::string name;
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    std::string cwd;
    std::string log;
    std::uint32_t everySeconds = 0;  // zero keeps the service running
    bool operator==(const ManagedService&) const = default;
};

// No partial replacement on error. A missing file is an empty manifest.
bool parseServiceManifest(std::string_view text, std::vector<ManagedService>& out, std::string& why);
bool readServiceManifest(const std::string& path, std::vector<ManagedService>& out, std::string& why);

} // namespace nexus::server
