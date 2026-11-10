// nexus-server: see server.h.
//
// Usage: nexus-server new [--dir DIR]     a session for a window: prints `NEXUS CONNECT <port> <key>` and exits
//        nexus-server serve [--dir DIR]   the server itself (new starts it when none runs)
//        nexus-server stop [--dir DIR]    stops the running server
//
// DIR is $HOME/.nexus unless given: the control socket server.sock and the log server.log live there.

#include <mach-o/dyld.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "server/server.h"

namespace {

int usage() {
    std::fprintf(stderr, "usage: nexus-server new|serve|stop [--dir DIR]\n");
    return 2;
}

// This program's own path, to start the server with.
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return usage();
    const std::string command = argv[1];
    std::string dir;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
            dir = argv[++i];
        else
            return usage();
    }
    nexus::server::Paths paths;
    std::string why;
    if (!nexus::server::paths(dir, &paths, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
    }
    if (command == "new")
        return nexus::server::newSession(paths, selfPath());
    if (command == "serve")
        return nexus::server::serve(paths);
    if (command == "stop")
        return nexus::server::stop(paths);
    return usage();
}
