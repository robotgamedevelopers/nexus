// nexus-server: see server.h.
//
// Usage: nexus-server new [--dir DIR] [--client ID --next N]
//                                         a session for a window: prints `NEXUS CONNECT <port> <key>` and exits; with
//                                         --client, the window's id and the number of its first action in the session;
//                                         NEXUS_STATE_FORMAT in its environment, the state format the window reads
//                                         (sync/formats.h), which the line then states after the key
//        nexus-server serve [--dir DIR]   the server itself (new starts it when none runs)
//        nexus-server stop [--dir DIR]    stops the running server
//        nexus-server setup [--no-service] [--wait S] [--dir DIR]
//                                         places this build and keeps it running (server/install.h, SETUP.md)
//        nexus-server doctor [--dir DIR]  what is missing, one line each, with the command that fixes it
//        nexus-server rebuild [--no-tests] [--jobs N] [--no-service] [--wait S] [--dir DIR]
//                                         builds the fork this was built from, runs the fast tests, then setup
//        nexus-server help [setup|doctor|rebuild|takeover]
//                                         each verb's help (server/install.h verbHelp); a verb with --help says it too
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
#include "server/install.h"
#include "server/server.h"
#include "server/takeover.h"
#include "sync/formats.h"

namespace {

const char* const kUsage =
    "usage: nexus-server new [--dir DIR] [--client ID --next N]\n"
    "       nexus-server serve|stop [--dir DIR]\n"
    "       nexus-server setup [--no-service] [--wait <seconds>] [--dir DIR]\n"
    "       nexus-server doctor [--dir DIR]\n"
    "       nexus-server rebuild [--no-tests] [--jobs <n>] [--no-service] [--wait <seconds>] [--dir DIR]\n"
    "       nexus-server config path|check|reload|history [--dir DIR]\n"
    "       nexus-server config restore <commit> | remote [<url>|--clear] [--dir DIR]\n"
    "       nexus-server help [setup|doctor|rebuild|takeover]\n"
    "  setup places this build and makes it the running server; doctor says what is missing, each with its fix;\n"
    "  rebuild builds your fork, tests it, then sets it up (SETUP.md; `nexus-server help <verb>` for each)\n"
    "  accounts and friends: DIR/nexus-config/accounts.json, friends.json (docs/accounts.md)\n"
    "ID is 1 to 64 letters, digits, '-' or '_'; N is a number from 1\n"
    "DIR ($HOME/.nexus/server unless given) holds the file drivers, read at each prompt, with lines\n"
    "  driver <harness> <program> [<argument>...]    and    friend <name> <harness> [<directory>]\n";

int usage() {
    std::fputs(kUsage, stderr);
    return 2;
}

// `nexus-server help [<topic>]`, or a verb with --help or -h: the help on standard output, exit 0.
int help(const std::string& topic) {
    const std::string text = nexus::server::verbHelp(topic);
    if (!topic.empty() && text.empty()) {
        std::fprintf(stderr, "nexus-server: no help on \"%s\"; the topics are setup, doctor, rebuild and takeover\n",
                     topic.c_str());
        return 2;
    }
    std::fputs(text.empty() ? kUsage : text.c_str(), stdout);
    return 0;
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
    if (command == "help" || command == "--help" || command == "-h") {
        return help(argc > 2 ? argv[2] : "");
    }
    for (int i = 2; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) &&
                !nexus::server::verbHelp(command).empty()) {
            return help(command);
        }
    }
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
        } else if (command == "config" || command == "setup" || command == "rebuild") {
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
        // The build this program is: the hash of its own bytes (server/takeover.h), which setup compares.
        std::string build;
        if (std::FILE* f = std::fopen(selfPath().c_str(), "rb")) {
            std::string bytes;
            char chunk[65536];
            for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) {
                bytes.append(chunk, n);
            }
            std::fclose(f);
            build = nexus::server::buildOf(bytes);
        }
        return nexus::server::serve(paths, build);
    }
    if (command == "setup") {
        return nexus::server::setup(paths, selfPath(), configArgs);
    }
    if (command == "doctor") {
        return nexus::server::doctor(paths, selfPath());
    }
    if (command == "rebuild") {
        return nexus::server::rebuild(paths, selfPath(), configArgs);
    }
    if (command == "stop")
        return nexus::server::stop(paths);
    if (command == "config") {
        return nexus::server::config(paths, configArgs);
    }
    return usage();
}
