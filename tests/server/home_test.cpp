// Unit tests of nexus-server's home and config (src/server/home.h): the home under HOME, no HOME, and the server's
// config over the settings files' parsing, whose own lines are tested with the window's code
// (tests/window/test_config.cpp). Logic only: the files' text is a string, and HOME is an argument.

#include <cstdio>
#include <string>
#include <vector>

#include "server/home.h"

namespace {

using namespace nexus;
using namespace nexus::server;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void home() {
    std::string resolved;
    std::string top;
    std::string dir;
    std::string why;
    CHECK(serverHome("/home/me", &resolved, &top, &dir, &why));
    CHECK(resolved == "/home/me" && top == "/home/me/.nexus" && dir == "/home/me/.nexus/server");
    CHECK(serverHome("/home/me//", &resolved, &top, &dir, &why));
    CHECK(resolved == "/home/me" && top == "/home/me/.nexus" && dir == "/home/me/.nexus/server");
    CHECK(serverHome("/", &resolved, &top, &dir, &why));
    CHECK(resolved == "/" && top == "/.nexus" && dir == "/.nexus/server");
    CHECK(!serverHome(nullptr, &resolved, &top, &dir, &why) && has(why, "HOME is not set"));
    why.clear();
    CHECK(!serverHome("", &resolved, &top, &dir, &why) && has(why, "HOME is not set"));
}

void serverConfig() {
    std::vector<std::string> warnings;
    std::string why;
    ServerSettings s;
    // None is fine, an unknown key a warning, a malformed line an error.
    CHECK(readServerConfig("# nothing yet\n", "/s/config", &s, &warnings, &why) && warnings.empty() && why.empty());
    CHECK(s.machine.empty());
    CHECK(readServerConfig("port 9\n", "/s/config", &s, &warnings, &why));
    CHECK(warnings.size() == 1 && has(warnings[0], "/s/config line 1:"));
    CHECK(!readServerConfig("\nport\n", "/s/config", &s, &warnings, &why));   // malformed, whatever the key: an error
    CHECK(warnings.empty() && has(why, "/s/config line 2:") && has(why, "no value"));
    CHECK(!readServerConfig("machine a\nmachine b\n", "/s/config", &s, &warnings, &why));
    CHECK(has(why, "/s/config line 2:") && has(why, "set again"));
    // machine: this machine's name in the fleet, a host name.
    CHECK(readServerConfig("machine studio\n", "/s/config", &s, &warnings, &why) && s.machine == "studio");
    CHECK(!readServerConfig("machine me@studio\n", "/s/config", &s, &warnings, &why) && has(why, "not a host name"));
    CHECK(s.machine.empty());

    // The server's machine: the config's, else the host name cut short; none when gethostname failed. An instance's
    // machine matches it in any case, and never an empty one.
    CHECK(serverMachine("studio", "Mac-Studio-2.local") == "studio");
    CHECK(serverMachine("", "Mac-Studio.local") == "Mac-Studio");
    CHECK(serverMachine("", "").empty());
    CHECK(sameMachine("studio", "studio") && sameMachine("Studio", "STUDIO"));
    CHECK(!sameMachine("studio", "studio-2") && !sameMachine("", "") && !sameMachine("studio", ""));
    // The full host name matches too, in any case: an instance may name `studio.local` or `studio`.
    CHECK(sameMachine("studio.local", "studio", "studio.local"));
    CHECK(sameMachine("Studio.Local", "studio", "studio.local"));
    CHECK(sameMachine("studio", "studio", "studio.local") && !sameMachine("studio.lan", "studio", "studio.local"));
    CHECK(!sameMachine("studio.local", "studio") && !sameMachine("", "studio", "") && !sameMachine("x", "", ""));

    // The logins: under HOME, whatever --dir says.
    CHECK(accountsDir("/h") == "/h/.nexus/server/accounts" && accountsDir("/h//") == "/h/.nexus/server/accounts");
}

} // namespace

int main() {
    home();
    serverConfig();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("home_test: all checks passed\n");
    return 0;
}
