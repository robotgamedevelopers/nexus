#include "server/home.h"

#include "config.h"

namespace nexus::server {

bool serverHome(const char* home, std::string* resolved, std::string* top, std::string* dir, std::string* why) {
    if (home == nullptr || *home == 0) {
        *why = "HOME is not set, so nexus-server has no home (HOME/.nexus/server) and no ~ for the drivers file or the "
            "accounts; start it with HOME set to your home directory";
        return false;
    }
    std::string h = home;
    while (h.size() > 1 && h.back() == '/') {
        h.pop_back();
    }
    *resolved = h;
    *top = (h == "/" ? std::string() : h) + "/.nexus";
    *dir = *top + "/server";
    return true;
}

bool readServerConfig(const std::string& text, const std::string& path, std::vector<std::string>* warnings,
        std::string* why) {
    const Config c = parseConfig(text, path, kServerConfigKeys);
    *warnings = c.warnings;
    why->clear();
    for (const std::string& e : c.errors) {
        *why += (why->empty() ? "" : "; ") + e;
    }
    return c.errors.empty();
}

} // namespace nexus::server
