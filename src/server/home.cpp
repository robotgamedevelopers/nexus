#include "server/home.h"

#include <algorithm>
#include <cctype>

#include "accounts/registry.h"
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

bool readServerConfig(const std::string& text, const std::string& path, ServerSettings* out,
        std::vector<std::string>* warnings, std::string* why) {
    const Config c = parseConfig(text, path, kServerConfigKeys);
    *warnings = c.warnings;
    why->clear();
    std::vector<std::string> errors = c.errors;
    *out = ServerSettings{};
    auto machine = c.values.find("machine");
    if (machine != c.values.end()) {
        if (accounts::isMachine(machine->second)) {
            out->machine = machine->second;
        } else {
            errors.push_back(path + ": machine " + machine->second + " is not a host name: 1 to 253 letters, digits, "
                "'-' or '.', not starting with '-' or '.', as a friend's instance names it (docs/accounts.md)");
        }
    }
    for (const std::string& e : errors) {
        *why += (why->empty() ? "" : "; ") + e;
    }
    return errors.empty();
}

std::string serverMachine(const std::string& configured, const std::string& hostName) {
    if (!configured.empty()) {
        return configured;
    }
    const std::string shortName = hostName.substr(0, hostName.find('.'));
    return accounts::isMachine(shortName) ? shortName : std::string();
}

bool sameMachine(const std::string& machine, const std::string& self) {
    return !self.empty() && machine.size() == self.size() &&
        std::equal(machine.begin(), machine.end(), self.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
}

} // namespace nexus::server
