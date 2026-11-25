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
    CHECK(s.machine.empty() && s.busLauncher.empty());
    CHECK(readServerConfig("bus-launcher /opt/nexus local/bin/bus\n", "/s/config", &s, &warnings, &why));
    CHECK(s.busLauncher == "/opt/nexus local/bin/bus");
    for (const auto* invalid : {"bus", "~/bus", "/", "/bin/", "/apps/../bin/bus", "/bin/./bus", "/bin/\tbus"}) {
        CHECK(!readServerConfig("bus-launcher " + std::string(invalid) + "\n", "/s/config", &s, &warnings, &why));
        CHECK(s.busLauncher.empty() && has(why, "absolute executable path"));
    }
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

    // dashboard: none by default; one line each, `<url> <name>`, kept in order, the name the rest of the line.
    CHECK(readServerConfig("machine studio\n", "/s/config", &s, &warnings, &why) && s.dashboards.empty());
    CHECK(readServerConfig("dashboard http://100.76.29.55:8080/ Dashboard\n", "/s/config", &s, &warnings, &why));
    CHECK((warnings.empty() && s.dashboards == std::vector<sync::Dashboard>{{"Dashboard", "http://100.76.29.55:8080/"}}));
    CHECK(readServerConfig("dashboard https://b.example/x Build farm\n# a note\ndashboard http://a:1/ Dashboard\n"
                           "dashboard http://c:2/  Third  one \n", "/s/config", &s, &warnings, &why));
    CHECK(s.dashboards == (std::vector<sync::Dashboard>{{"Build farm", "https://b.example/x"},
                              {"Dashboard", "http://a:1/"}, {"Third  one", "http://c:2/"}}));
    // Each bad line is an error naming its line, and the good lines stand.
    CHECK(!readServerConfig("dashboard http://a:1/\n", "/s/config", &s, &warnings, &why));
    CHECK(has(why, "/s/config line 1:") && has(why, "dashboard <url> <name>") && s.dashboards.empty());
    CHECK(!readServerConfig("dashboard ftp://a/ A\ndashboard http://b:1/ B\n", "/s/config", &s, &warnings, &why));
    CHECK(has(why, "line 1:") && has(why, "not http:// or https://"));
    CHECK((s.dashboards == std::vector<sync::Dashboard>{{"B", "http://b:1/"}}));
    CHECK(!readServerConfig("dashboard http:// A\n", "/s/config", &s, &warnings, &why) && has(why, "no host"));
    CHECK(!readServerConfig("dashboard http://a:1/ A\ndashboard http://b:1/ A\n", "/s/config", &s, &warnings, &why));
    CHECK(has(why, "line 2:") && has(why, "given twice") && s.dashboards.size() == 1);
    CHECK(!readServerConfig("dashboard http://a:1/ " + std::string(sync::kDashboardNameMax + 1, 'n') + "\n",
        "/s/config", &s, &warnings, &why) && has(why, "1 to 64 bytes"));
    CHECK(!readServerConfig("dashboard http://a:1/ A\x01" "B\n", "/s/config", &s, &warnings, &why));
    CHECK(has(why, "control character"));
    CHECK(!readServerConfig("dashboard http://a/" + std::string(sync::kDashboardUrlMax, 'x') + " A\n", "/s/config", &s,
        &warnings, &why) && has(why, "longer than"));
    std::string many;
    for (std::size_t i = 0; i <= sync::kDashboardsMax; ++i)
        many += "dashboard http://h:1/ D" + std::to_string(i) + "\n";
    CHECK(!readServerConfig(many, "/s/config", &s, &warnings, &why) && has(why, "more than 16 dashboards"));
    CHECK(s.dashboards.size() == sync::kDashboardsMax);

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

    // The sandbox's tools: beside the server, from its own path only; nothing from a path that is not absolute.
    CHECK(sandboxBinOf("/opt/nexus/build/nexus-server") == "/opt/nexus/build/sandbox");
    CHECK(sandboxBinOf("/nexus-server") == "/sandbox");
    CHECK(sandboxBinOf("").empty() && sandboxBinOf("nexus-server").empty() && sandboxBinOf("build/nexus-server").empty());
    CHECK(sandboxBinOf("/opt/nexus/build/").empty());
    CHECK(std::string(kSandboxBinVariable) == "NEXUS_SANDBOX_BIN");
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
