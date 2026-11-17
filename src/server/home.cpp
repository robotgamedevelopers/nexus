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
    auto claude = c.values.find("claude");
    if (claude != c.values.end()) {
        if (!claude->second.empty() && claude->second[0] == '/') {
            out->claude = claude->second;
        } else {
            errors.push_back(path + ": claude " + claude->second + " is not an absolute path to the claude program");
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

namespace {
bool sameName(const std::string& a, const std::string& b) {
    return !b.empty() && a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
} // namespace

bool sameMachine(const std::string& machine, const std::string& self, const std::string& hostName) {
    return sameName(machine, self) || sameName(machine, hostName);
}

std::string accountsDir(const std::string& home) {
    std::string h = home;
    while (!h.empty() && h.back() == '/') {
        h.pop_back();
    }
    return h + "/" + kAccountsUnderHome;
}

std::string sandboxBinOf(const std::string& self) {
    const std::string::size_type slash = self.rfind('/');
    if (self.empty() || self.front() != '/' || slash == std::string::npos || slash + 1 == self.size()) {
        return std::string();
    }
    return self.substr(0, slash) + "/sandbox";
}

} // namespace nexus::server
