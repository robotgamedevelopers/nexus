// nexus-server: see server.h.
//
// Usage: nexus-server new [--dir DIR] [--client ID --next N]
//                                         a session for a window: prints `NEXUS CONNECT <port> <key>` and exits; with
//                                         --client, the window's id and the number of its first action in the session;
//                                         NEXUS_STATE_FORMAT in its environment, the state format the window reads
//                                         (sync/formats.h), which the line then states after the key
//        nexus-server serve [--dir DIR]   the server itself (new starts it when none runs)
//        nexus-server stop [--dir DIR]    stops the running server
//        nexus-server config <verb> [--dir DIR]
//                                         the accounts and friends, DIR/nexus-config/accounts.json and friends.json
//                                         (docs/accounts.md): path, check, reload, history, restore <commit>,
//                                         remote [<url> | --clear]
//
// DIR is $HOME/.nexus/server unless given (server/home.h): the settings file config, the control socket server.sock,
// the log server.log and the store store/ live there, and the file drivers, which names each friend's driver
// (server/driver_table.h):
//   driver <harness> <program> [<argument>...]
//   friend <name> <harness> [<directory>]
// The harness logins, accounts/<name>/, are always under $HOME/.nexus/server, whatever DIR is: a driver finds its
// account there by its --account.

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "no_core.h"
#include "server/home.h"
#include "server/server.h"
#include "sync/formats.h"

namespace {

int usage() {
    std::fprintf(stderr, "usage: nexus-server new [--dir DIR] [--client ID --next N]\n"
                         "       nexus-server serve|stop [--dir DIR]\n"
                         "       nexus-server config path|check|reload|history [--dir DIR]\n"
                         "       nexus-server config restore <commit> | remote [<url>|--clear] [--dir DIR]\n"
                         "  accounts and friends: DIR/nexus-config/accounts.json, friends.json (docs/accounts.md)\n"
                         "ID is 1 to 64 letters, digits, '-' or '_'; N is a number from 1\n"
                         "DIR ($HOME/.nexus/server unless given) holds the file drivers, read at each prompt, with lines\n"
                         "  driver <harness> <program> [<argument>...]    and    friend <name> <harness> [<directory>]\n");
    return 2;
}

// This program's own path, to start the server with.
#if defined(__APPLE__)
std::string selfPath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return std::string();
    char* real = realpath(buf.data(), nullptr);
    if (real == nullptr)
        return std::string(buf.data());
    std::string out(real);
    std::free(real);
    return out;
}
#else
// /proc/self/exe itself when its target cannot be resolved: it still starts this program, even one replaced on disk.
// Empty, with the cause on standard error, only when it cannot be read at all.
std::string selfPath() {
    const char* const exe = "/proc/self/exe";
    char* real = realpath(exe, nullptr);
    if (real != nullptr) {
        std::string out(real);
        std::free(real);
        return out;
    }
    const int realpathError = errno;
    if (access(exe, X_OK) == 0) {
        return exe;
    }
    std::fprintf(stderr, "nexus-server: cannot find this program's own path to start the server with: realpath(%s): "
        "%s (is /proc mounted?)\n", exe, std::strerror(realpathError));
    return std::string();
}
#endif

} // namespace

int main(int argc, char** argv) {
    // No core file holds a secret this process wrote (no_core.h).
    if (!nexus::noCoreDumps()) {
        std::fprintf(stderr, "nexus-server: core dumps could not be turned off: %s\n", std::strerror(errno));
    }
    if (argc < 2)
        return usage();
    const std::string command = argv[1];
    std::string dir;
    nexus::server::Client client;
    bool haveNext = false;
    std::vector<std::string> configArgs;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (command == "new" && std::strcmp(argv[i], "--client") == 0 && i + 1 < argc) {
            client.id = argv[++i];
        } else if (command == "new" && std::strcmp(argv[i], "--next") == 0 && i + 1 < argc) {
            char* end = nullptr;
            client.next = std::strtoull(argv[++i], &end, 10);
            haveNext = true;
            if (end == argv[i] || *end != 0 || client.next == 0)
                return usage();
        } else if (command == "config") {
            configArgs.push_back(argv[i]);
        } else {
            return usage();
        }
    }
    if (haveNext != !client.id.empty() || (!client.id.empty() && !nexus::server::validClientId(client.id)))
        return usage();
    nexus::server::Paths paths;
    std::string why;
    if (!nexus::server::paths(dir, &paths, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
    }
    if (command == "new") {
        // The state format the window reads, which it states in the environment (sync/formats.h); unset or not a
        // number from 1, the window states none and reads format 1.
        if (const char* stated = std::getenv(nexus::sync::kStateFormatVariable)) {
            char* end = nullptr;
            const unsigned long format = std::strtoul(stated, &end, 10);
            if (end != stated && *end == 0 && format != 0 && format < 1000000000UL) {
                client.format = static_cast<std::uint32_t>(format);
            }
        }
        return nexus::server::newSession(paths, selfPath(), client);
    }
    if (command == "serve") {
        // The sandbox's bwrap and socat that the build made beside this program on Linux (cmake/sandbox.cmake), for
        // the Claude driver to find (drivers/claude/src/tools.ts; on macOS it only keeps tools from writing there):
        // from this program's own path only, over any value it was started with (server/home.h sandboxBinOf).
        const std::string bin = nexus::server::sandboxBinOf(selfPath());
        if (bin.empty()) {
            unsetenv(nexus::server::kSandboxBinVariable);
        } else {
            setenv(nexus::server::kSandboxBinVariable, bin.c_str(), 1);
        }
        return nexus::server::serve(paths);
    }
    if (command == "stop")
        return nexus::server::stop(paths);
    if (command == "config") {
        return nexus::server::config(paths, configArgs);
    }
    return usage();
}
