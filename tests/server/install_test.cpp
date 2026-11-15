// Unit tests of setup's pure parts (src/server/install.h): where each thing is placed, the launchd plist and the systemd
// unit it registers, the window's `server local`, the first drivers file, and the move of driver paths out of the
// fork. Logic only: no files, processes, sockets or clock.

#include <cstdio>
#include <string>

#include "server/driver_table.h"
#include "server/install.h"

namespace {

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

void placements() {
    const Placement at = placement("/home/p", "/home/p");
    CHECK(at.top == "/home/p/.nexus");
    CHECK(at.server == "/home/p/.nexus/bin/nexus-server");
    CHECK(at.driver == "/home/p/.nexus/drivers/claude");
    CHECK(at.client == "/home/p/.nexus/client");
    CHECK(at.app == "/home/p/Applications/nexus.app");
    CHECK(at.localBin == "/home/p/.local/bin/nexus-server");
#if defined(__APPLE__)
    CHECK(at.serviceName == "com.robotgamedevelopers.nexus-server");
    CHECK(at.serviceFile == "/home/p/Library/LaunchAgents/com.robotgamedevelopers.nexus-server.plist");
#else
    CHECK(at.serviceName == "nexus-server");
    CHECK(at.serviceFile == "/home/p/.config/systemd/user/nexus-server.service");
#endif
    // Another HOME than the user's gets a service of its own, the same one each time.
    const Placement other = placement("/tmp/h", "/home/p");
    const Placement again = placement("/tmp/h", "/home/p");
    const Placement third = placement("/tmp/i", "/home/p");
    CHECK(other.serviceName != at.serviceName && other.serviceName == again.serviceName);
    CHECK(third.serviceName != other.serviceName);
    CHECK(has(other.serviceFile, other.serviceName) && other.serviceFile.rfind("/tmp/h/", 0) == 0);
}

void plist() {
    const std::string text = launchdPlist("L", "/h/.nexus/bin/nexus-server", "/h/.nexus/server", "/h/log", "/h",
                                          "/usr/bin:/a&b");
    CHECK(has(text, "<key>Label</key>\n    <string>L</string>"));
    CHECK(has(text, "<string>/h/.nexus/bin/nexus-server</string>\n        <string>serve</string>\n"
                    "        <string>--dir</string>\n        <string>/h/.nexus/server</string>"));
    // Restarted after any exit but a clean one: a takeover's exit 75 restarts it, `stop` does not.
    CHECK(has(text, "<key>KeepAlive</key>\n    <dict>\n        <key>SuccessfulExit</key>\n        <false/>"));
    CHECK(has(text, "<key>RunAtLoad</key>\n    <true/>"));
    CHECK(has(text, "<key>HOME</key>\n        <string>/h</string>"));
    // Text is escaped for XML.
    CHECK(has(text, "<string>/usr/bin:/a&amp;b</string>") && !has(text, "/a&b"));
}

void unit() {
    const std::string text = systemdUnit("/h/my dir/nexus-server", "/h/.nexus/server", "/h", "/usr/bin:/50%");
    CHECK(has(text, "ExecStart=\"/h/my dir/nexus-server\" serve --dir \"/h/.nexus/server\"\n"));
    CHECK(has(text, "Environment=\"HOME=/h\" \"PATH=/usr/bin:/50%%\"\n"));
    CHECK(has(text, "Restart=on-failure\n"));
    CHECK(has(text, "WantedBy=default.target\n"));
}

void clientConfig() {
    CHECK(withLocalServer("") == withLocalServer(std::string()));
    CHECK(has(withLocalServer(""), "\nserver local\n"));
    // Added after what is there, with its newline.
    const std::string added = withLocalServer("theme dark");
    CHECK(added.rfind("theme dark\n", 0) == 0 && has(added, "server local\n"));
    // A server already named is kept; a comment or another key that starts the same is not one.
    CHECK(withLocalServer("server studio\n") == "server studio\n");
    CHECK(withLocalServer("  server local\n") == "  server local\n");
    CHECK(has(withLocalServer("# server studio\n"), "\nserver local\n"));
    CHECK(has(withLocalServer("servers 2\n"), "\nserver local\n"));
    // Twice is once.
    CHECK(withLocalServer(withLocalServer("a 1\n")) == withLocalServer("a 1\n"));
}

void drivers() {
    const std::string first = firstDriversFile("/usr/bin/node", "/h/.nexus/drivers/claude/dist/src/main.js");
    CHECK(has(first, "\ndriver claude /usr/bin/node /h/.nexus/drivers/claude/dist/src/main.js --account main\n"));
    // A first install's one friend: claude, on the account main, in the home directory, as the server first reads it.
    CHECK(has(first, "\nfriend claude claude\n"));
    const DriverTable parsed = parseDriverTable(first, "/h");
    CHECK(parsed.errors.empty());
    CHECK(parsed.friends.size() == 1 && parsed.friends.count("claude") == 1 && parsed.friends.at("claude").cwd == "/h");
    const nexus::accounts::Registry seed = accountsFromDrivers(parsed, "box");
    CHECK(seed.accounts.size() == 1 && seed.accounts[0].name == "main" && seed.accounts[0].provider == "claude");
    CHECK(seed.friends.size() == 1 && seed.friends[0].name == "claude" && seed.friends[0].preferred == "main");
    const std::string table = "driver claude /n /src/nexus/drivers/claude/dist/src/main.js --account main\n"
                              "friend a claude\n"
                              "driver other /n /src/nexus/drivers/claude/dist/src/main.js\n";
    const std::string moved = movedDriverPaths(table, "/src/nexus", "/h/.nexus/drivers/claude");
    CHECK(moved == "driver claude /n /h/.nexus/drivers/claude/dist/src/main.js --account main\n"
                   "friend a claude\n"
                   "driver other /n /h/.nexus/drivers/claude/dist/src/main.js\n");
    // Nothing into the fork: unchanged; and moving twice is moving once.
    CHECK(movedDriverPaths(moved, "/src/nexus", "/h/.nexus/drivers/claude") == moved);
    CHECK(movedDriverPaths("driver x /n /elsewhere/main.js\n", "/src/nexus", "/p") == "driver x /n /elsewhere/main.js\n");
}

void builds() {
    // With no driver tree, the build is the program's; with one, it is of both, so a new driver alone is a new build.
    const nexus::server::DriverPin none;
    const nexus::server::DriverPin one{"/h/.nexus/drivers/claude", "/h/.nexus/drivers/claude@0123456789abcdef"};
    const nexus::server::DriverPin two{"/h/.nexus/drivers/claude", "/h/.nexus/drivers/claude@fedcba9876543210"};
    CHECK(runningBuild("aaaaaaaaaaaaaaaa", none) == "aaaaaaaaaaaaaaaa");
    CHECK(runningBuild("aaaaaaaaaaaaaaaa", one) != "aaaaaaaaaaaaaaaa");
    CHECK(runningBuild("aaaaaaaaaaaaaaaa", one) != runningBuild("aaaaaaaaaaaaaaaa", two));
    CHECK(runningBuild("aaaaaaaaaaaaaaaa", one) != runningBuild("bbbbbbbbbbbbbbbb", one));
    CHECK(runningBuild("aaaaaaaaaaaaaaaa", one).size() == 16);
    // Only the tree's name counts, not where HOME is.
    const nexus::server::DriverPin moved{"/g/.nexus/drivers/claude", "/g/.nexus/drivers/claude@0123456789abcdef"};
    CHECK(runningBuild("aaaaaaaaaaaaaaaa", one) == runningBuild("aaaaaaaaaaaaaaaa", moved));
}

} // namespace

int main() {
    placements();
    plist();
    unit();
    clientConfig();
    drivers();
    builds();
    if (failures != 0) {
        std::fprintf(stderr, "install_test: %d failed\n", failures);
        return 1;
    }
    std::printf("install_test: ok\n");
    return 0;
}
