// nexus-server: see server.h.
//
// Usage: nexus-server new [--dir DIR] [--client ID --next N]
//                                         a session for a window: prints `NEXUS CONNECT <port> <key>` and exits; with
//                                         --client, the window's id and the number of its first action in the session
//        nexus-server serve [--dir DIR]   the server itself (new starts it when none runs)
//        nexus-server stop [--dir DIR]    stops the running server
//
// DIR is $HOME/.nexus unless given: the control socket server.sock, the log server.log and the store store/ live there,
// and the file drivers, which names each friend's driver (server/driver_table.h):
//   driver <harness> <program> [<argument>...]
//   friend <name> <harness> [<directory>]

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "server/server.h"

namespace {

int usage() {
    std::fprintf(stderr, "usage: nexus-server new [--dir DIR] [--client ID --next N]\n"
                         "       nexus-server serve|stop [--dir DIR]\n"
                         "ID is 1 to 64 letters, digits, '-' or '_'; N is a number from 1\n"
                         "DIR ($HOME/.nexus unless given) holds the file drivers, read at each prompt, with lines\n"
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
std::string selfPath() {
    char* real = realpath("/proc/self/exe", nullptr);
    if (real == nullptr) {
        return std::string();
    }
    std::string out(real);
    std::free(real);
    return out;
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return usage();
    const std::string command = argv[1];
    std::string dir;
    nexus::server::Client client;
    bool haveNext = false;
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
    if (command == "new")
        return nexus::server::newSession(paths, selfPath(), client);
    if (command == "serve")
        return nexus::server::serve(paths);
    if (command == "stop")
        return nexus::server::stop(paths);
    return usage();
}
