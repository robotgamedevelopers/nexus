// The in-app ssh (src/link/inapp_ssh.h) end to end, on a simulator device, against a real sshd on 127.0.0.1 and the
// real tools/nexus-ios-gate (tests/ios/sshd_fixture.sh, started by ctest before this): the link's bootstrap reads the
// CONNECT line a stand-in nexus-server prints; the server's host key is kept on first use and checked after; the gate
// refuses what is not the link's; a host key that changed is refused; and a key the server does not hold is refused,
// saying which line to add. A functional test: real sockets, on loopback only.
//
// Usage: ios_ssh_functional DIR      (the fixture's directory: DIR/port, DIR/user, DIR/client/ssh-key)

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "link/bootstrap.h"
#include "link/inapp_ssh.h"
#include "sync/formats.h"

namespace {

using namespace nexus;
using namespace nexus::link;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream s;
    s << in.rdbuf();
    std::string text = s.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ios_ssh_functional DIR\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string user = slurp(dir + "/user");
    const int port = std::atoi(slurp(dir + "/port").c_str());
    if (user.empty() || port <= 0) {
        std::fprintf(stderr, "ios_ssh_functional: no fixture in %s (tests/ios/sshd_fixture.sh start)\n", dir.c_str());
        return 2;
    }
    InAppSshOptions o;
    o.dir = dir + "/client";
    o.port = port;
    const CommandRunner run = inAppSsh(o);
    Bootstrap b;
    b.host = user + "@127.0.0.1";
    b.client = "window-0123456789abcdef";
    b.timeoutSeconds = 20;

    // The bootstrap, through the gate, twice: the host key is kept the first time and matches the second.
    for (int pass = 0; pass < 2; ++pass) {
        Connect connect;
        std::string why;
        const bool ok = bootstrap(b, run, &connect, &why);
        if (!ok) std::fprintf(stderr, "ios_ssh_functional: pass %d: %s\n", pass, why.c_str());
        CHECK(ok);
        CHECK(connect.port == "60001" && connect.key == "AbCdEfGhIjKlMnOpQrSt+/");
        CHECK(connect.format == b.stateFormat);
    }
    const std::string known = slurp(o.dir + "/known_hosts");
    CHECK(known.rfind("[127.0.0.1]:" + std::to_string(port) + " ecdsa-sha2-nistp256 AAAA", 0) == 0);
    CHECK(known.find('\n') == std::string::npos);   // one line: the second pass added none

    // A history page passes the gate and reaches the stand-in, which takes only new: its status and words come back.
    Bootstrap history = b;
    history.command = {"nexus-server", "history-page", "1", "2", "3"};
    history.client.clear();
    const CommandResult page = run(bootstrapCommand(history), 20);
    CHECK(page.end == CommandResult::End::Exited && page.code == 3);
    CHECK(page.err.find("fixture nexus-server: history-page 1 2 3") != std::string::npos);

    // The gate refuses what is not the link's, with its own status.
    const CommandResult refused = run(Command{"/usr/bin/ssh", {"-T", b.host, "--", "exec \"$SHELL\" -l -c 'id'"},
        "ssh id"}, 20);
    CHECK(refused.end == CommandResult::End::Exited && refused.code == 126);
    CHECK(refused.err.find("nexus-ios-gate:") != std::string::npos);

    // A host key that is not the one kept is refused.
    {
        std::ofstream changed(o.dir + "/known_hosts");
        changed << "[127.0.0.1]:" << port << " ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTY=\n";
    }
    Connect connect;
    std::string why;
    CHECK(!bootstrap(b, run, &connect, &why));
    CHECK(why.find("host key is not the one kept") != std::string::npos);

    // A device key the server does not hold: refused, saying the line to add.
    unlink((o.dir + "/known_hosts").c_str());
    unlink((o.dir + "/ssh-key").c_str());
    CHECK(!bootstrap(b, run, &connect, &why));
    CHECK(why.find("did not take this device's key") != std::string::npos);
    CHECK(why.find("ecdsa-sha2-nistp256 AAAA") != std::string::npos);

    if (failures != 0) {
        std::fprintf(stderr, "ios_ssh_functional: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ios_ssh_functional: all passed\n");
    return 0;
}
