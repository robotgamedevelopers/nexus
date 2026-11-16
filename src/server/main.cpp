// nexus-server: see server.h.
//
// Usage: nexus-server new [--dir DIR] [--client ID --next N]
//                                         a session for a window: prints `NEXUS CONNECT <port> <key>` and exits; with
//                                         --client, the window's id and the number of its first action in the session;
//                                         NEXUS_STATE_FORMAT in its environment, the state format the window reads
//                                         (sync/formats.h), which the line then states after the key; with
//                                         NEXUS_ASK_RUNNING=1 there, it asks only a running server and starts none
//                                         (a window reconnecting: link/bootstrap.h)
//        nexus-server serve [--dir DIR]   the server itself (new starts it when none runs); exits 0, saying so, only
//                                         when another server answers that it is serving; 3 when the store's lock is
//                                         held for the whole wait by a server neither serving nor ending; 1 when it
//                                         could not start (readers that did not let go among the reasons)
//        nexus-server store check [--dir DIR]
//                                         reads the store without changing it; refused while a server has it
//        nexus-server stop [--dir DIR]    stops the running server
//        nexus-server drain [--dir DIR]   asks the running server to finish open turns and exit for a handover
//        nexus-server status [--dir DIR]  asks the running server: SERVING, and the build it runs from
//        nexus-server builds <verb> ... [--dir DIR]
//                                         the installed builds under DIR/builds (server/builds.h, docs/builds.md):
//                                         place, hash, formats, list, verify, current, back, prune, remove, help
//        nexus-server handover [<hash>|resume|status|help] [--dir DIR]
//                                         replaces the running server with a placed build under an OS lock, or
//                                         resumes an interrupted handover (server/handover.h, docs/builds.md)
//        nexus-server config <verb> [--dir DIR]
//                                         the accounts and friends, DIR/nexus-config/accounts.json and friends.json
//                                         (docs/accounts.md): path, check, reload, history, restore <commit>,
//                                         remote [<url> | --clear]; and sign-in <account>, which waits while the
//                                         person signs the account in in the browser from nexus's window, and prints
//                                         `signed in` or `refused: <why>` (docs/secrets.md "Sign-in")
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

#include "link/bootstrap.h"
#include "no_core.h"
#include "server/builds.h"
#include "server/handover.h"
#include "server/home.h"
#include "server/server.h"
#include "sync/formats.h"

namespace {

int usage() {
    std::fprintf(stderr, "usage: nexus-server new [--dir DIR] [--client ID --next N]\n"
                         "       nexus-server serve|stop|drain|status [--dir DIR]\n"
                         "       nexus-server builds <verb> ... [--dir DIR]   (`nexus-server builds help`)\n"
                         "       nexus-server handover [<hash>|resume|status|help] [--dir DIR]\n"
                         "       nexus-server config path|check|reload|history [--dir DIR]\n"
                         "       nexus-server config restore <commit> | remote [<url>|--clear] [--dir DIR]\n"
                         "       nexus-server config sign-in <account> [--dir DIR]\n"
                         "       nexus-server store check [--dir DIR]\n"
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
        } else if (command == "config" || command == "store" || command == "builds" || command == "handover") {
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
        // Its Hello (sync/formats.h), from a window of format 4 on. One that does not read is said and left out: the
        // server then goes by the format alone, as with a window from before Hello.
        if (const char* hello = std::getenv(nexus::sync::kHelloVariable)) {
            std::string helloWhy;
            if (nexus::sync::readHello(hello, &client.hello, &helloWhy)) {
                client.hasHello = true;
            } else {
                std::fprintf(stderr, "nexus-server: %s is %s; going by the state format alone\n",
                    nexus::sync::kHelloVariable, helloWhy.c_str());
            }
        }
        // A window that has heard a server asks only a running one (link/bootstrap.h kAskRunningVariable): a server
        // stopped on purpose stays stopped, and a replacement build comes up by the service, never by a window's
        // reconnect.
        const char* askRunning = std::getenv(nexus::link::kAskRunningVariable);
        const bool start = askRunning == nullptr || std::strcmp(askRunning, "1") != 0;
        return nexus::server::newSession(paths, start ? selfPath() : std::string(), client);
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
        // The build this program is in, held for the server's life so that no prune removes it while it runs; one
        // being removed as this starts refuses the start.
        nexus::server::builds::BuildHold hold;
        const std::string self = selfPath();
        const nexus::store::Status held = nexus::server::builds::holdBuild(self, hold);
        if (!held.is_ok()) {
            std::fprintf(stderr, "nexus-server: %s\n", held.message().c_str());
            return 1;
        }
        paths.build = hold.hash();
        paths.self = self;
        return nexus::server::serve(paths);
    }
    if (command == "stop") {
        return nexus::server::stop(paths);
    }
    if (command == "drain") {
        return nexus::server::drain(paths);
    }
    if (command == "status") {
        return nexus::server::status(paths);
    }
    if (command == "builds") {
        nexus::server::builds::Result r;
        const int code = nexus::server::builds::command(paths.dir, configArgs, {}, r);
        for (const std::string& line : r.out) {
            std::printf("%s\n", line.c_str());
        }
        std::fflush(stdout);
        for (const std::string& line : r.err) {
            std::fprintf(stderr, "nexus-server: builds: %s\n", line.c_str());
        }
        return code;
    }
    if (command == "handover") {
        nexus::server::handover::Result r;
        const int code = nexus::server::handover::command(paths.dir, configArgs, {}, r);
        for (const std::string& line : r.out) {
            std::printf("%s\n", line.c_str());
        }
        std::fflush(stdout);
        for (const std::string& line : r.err) {
            std::fprintf(stderr, "nexus-server: handover: %s\n", line.c_str());
        }
        return code;
    }
    if (command == "config") {
        return nexus::server::config(paths, configArgs);
    }
    if (command == "store") {
        return nexus::server::storeCommand(paths, configArgs);
    }
    return usage();
}
