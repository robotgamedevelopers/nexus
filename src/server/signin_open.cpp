// nexus-signin-open: nexus's stand-in for a browser in in-app sign-in (server/signin_runner.h, docs/secrets.md
// "Sign-in"). `claude setup-token` runs it as `open`, `xdg-open` and the like (first on its PATH) and as BROWSER: it
// hands the URL to nexus-server over the socket NEXUS_SIGNIN_SOCKET names, with the flow's word NEXUS_SIGNIN_FLOW gives,
// and opens nothing. It writes nothing to its output or error, the URL least of all: the program's terminal is the
// server's, and what it shows goes nowhere else.
//
// The URL is its last argument that starts with http:// or https:// (`open -a <app> <url>` takes options before it).
// Exits 0 once the server has the line, else 1.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

bool writeAll(int fd, const std::string& bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + at, bytes.size() - at);
        if (n <= 0) {
            return false;
        }
        at += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const char* socketPath = std::getenv("NEXUS_SIGNIN_SOCKET");
    const char* flow = std::getenv("NEXUS_SIGNIN_FLOW");
    const char* url = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "https://", 8) == 0 || std::strncmp(argv[i], "http://", 7) == 0) {
            url = argv[i];
        }
    }
    if (socketPath == nullptr || flow == nullptr || url == nullptr) {
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (std::strlen(socketPath) >= sizeof addr.sun_path) {
        return 1;
    }
    std::memcpy(addr.sun_path, socketPath, std::strlen(socketPath) + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        return 1;
    }
    std::string line = std::string(flow) + " " + url + "\n";
    const bool ok = writeAll(fd, line);
    std::memset(line.data(), 0, line.size());
    ::close(fd);
    return ok ? 0 : 1;
}
