// A FAKE `claude`, for in-app sign-in's functional tests (tests/server/signin_functional.cpp): it stands in for
// `claude setup-token` as nexus-server's runner starts it, and touches no real login, page or network.
//
// Run as `<name> setup-token`. Its name says what it does (the runner gives it an environment of its own, so the test
// cannot pass it a variable): `claude-<mode>[-<port>]`.
//
// - It checks what the runner promises: a terminal on 0, 1 and 2; CLAUDE_CONFIG_DIR, an existing directory; BROWSER
//   set; no ANTHROPIC_API_KEY, ANTHROPIC_AUTH_TOKEN or CLAUDE_CODE_OAUTH_TOKEN. Any broken: it exits 3.
// - It listens on loopback (on <port> with one, else any port), on 127.0.0.1 alone, and makes up a sign-in URL whose
//   redirect_uri is http://localhost:<that port>/callback; with mode `forward` the redirect names another, free port,
//   as on another machine, and the test's replay port is <port>; `forward6` is the same listening on [::1] alone.
// - It hands the URL to $BROWSER (modes `helper`, `forward`, `forward6`, `deny`, `apikey`, `quick`, `group`), or prints
//   it in its terminal (`print`), or does both (`both`), the way the real program falls back; `silent` gives it to
//   nobody.
// - With mode `quick` it exits the moment its token is printed; with `group` it first starts a process of its own, in
//   its process group, that ignores a hangup (its pid in grandchild.pid beside the program's name).
// - It takes the one GET on /callback: with its state and code `fakeCode...`, it prints a made-up token (an API key's
//   shape with `apikey`), between terminal escapes, and exits 0; with `error=...` it prints that sign-in was declined
//   and exits 1; any other request (a wrong path or state) ends it: exit 5, as a wrong request would end a real login.
// - With mode `late` it prints its token only once it is told to end (SIGTERM), as a program whose flow's deadline
//   passed while the browser was sent back.
// - With no callback in 60 s it exits 4, so that no test hangs.
//
// The values are made up (FAKE in each) and of letters the tests find in nothing else (tests/leak_scan.h).

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "fake_claude.h"

extern char** environ;

namespace {

using nexus::fake_claude::kApiKey;
using nexus::fake_claude::kCode;
using nexus::fake_claude::kState;
using nexus::fake_claude::kToken;

int listenOn(std::uint16_t port, std::uint16_t* bound) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0 ||
            getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        close(s);
        return -1;
    }
    *bound = ntohs(a.sin_port);
    return s;
}

// On [::1] alone (IPV6_V6ONLY), as a program whose `localhost` resolved to IPv6 first does.
int listenOn6(std::uint16_t port, std::uint16_t* bound) {
    const int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    a.sin6_addr = in6addr_loopback;
    socklen_t len = sizeof a;
    if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0 ||
            getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        close(s);
        return -1;
    }
    *bound = ntohs(a.sin6_port);
    return s;
}

// A process of the program's own (as a real one may leave a helper running), in its process group, that ignores the
// hangup its terminal's end sends: only a signal to the group ends it. Its pid goes to grandchild.pid beside the
// program's name, for the test to watch.
void spawnGrandchild(const std::string& self) {
    const pid_t pid = fork();
    if (pid == 0) {
        signal(SIGHUP, SIG_IGN);
        for (int i = 0; i < 600; ++i) {
            usleep(100000);
        }
        _exit(0);
    }
    if (pid > 0) {
        const std::string path = self.substr(0, self.rfind('/') + 1) + "grandchild.pid";
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            std::fprintf(f, "%ld\n", static_cast<long>(pid));
            std::fclose(f);
        }
    }
}

bool terminal(int fd) { return isatty(fd) == 1; }

bool environmentAsPromised() {
    const char* dir = std::getenv("CLAUDE_CONFIG_DIR");
    struct stat st{};
    if (!terminal(0) || !terminal(1) || !terminal(2) || fcntl(3, F_GETFL) < 0 || dir == nullptr ||
            stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || std::getenv("BROWSER") == nullptr ||
            std::getenv("ANTHROPIC_API_KEY") != nullptr || std::getenv("ANTHROPIC_AUTH_TOKEN") != nullptr ||
            std::getenv("CLAUDE_CODE_OAUTH_TOKEN") != nullptr) {
        return false;
    }
    for (int fd = 4; fd < 64; ++fd) {
        if (fcntl(fd, F_GETFL) != -1) {
            return false;
        }
    }
    return true;
}

void openInBrowser(const std::string& url) {
    const char* browser = std::getenv("BROWSER");
    if (browser == nullptr) {
        return;
    }
    std::string program = browser;
    std::string arg = url;
    char* argv[] = {program.data(), arg.data(), nullptr};
    pid_t pid = -1;
    if (posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv, environ) == 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* config = std::getenv("CLAUDE_CONFIG_DIR");
    const bool authLogin = argc == 6 && std::strcmp(argv[1], "auth") == 0 && std::strcmp(argv[2], "login") == 0 &&
        std::strcmp(argv[3], "--claudeai") == 0 && std::strcmp(argv[4], "--email") == 0 && config;
    if (argc == 4 && std::strcmp(argv[1], "auth") == 0 && std::strcmp(argv[2], "status") == 0 &&
            std::strcmp(argv[3], "--json") == 0 && config) {
        std::ifstream identity(std::string(config) + "/test-browser-email");
        std::string email;
        if (!(identity >> email)) return 3;
        std::printf(R"({"loggedIn":true,"authMethod":"claude.ai","apiProvider":"firstParty","email":"%s"})" "\n",
                    email.c_str());
        return 0;
    }
    if (!authLogin && (argc != 2 || std::strcmp(argv[1], "setup-token") != 0)) {
        return 2;
    }
    if (!authLogin && !environmentAsPromised()) {
        std::printf("fake claude: the environment is not as the runner promises\r\n");
        return 3;
    }
    // claude-<mode>[-<port>]
    std::string name = argv[0];
    name = name.substr(name.rfind('/') + 1);
    std::string mode = name.substr(name.find('-') + 1);
    std::uint16_t port = 0;
    const std::size_t dash = mode.find('-');
    if (dash != std::string::npos) {
        port = static_cast<std::uint16_t>(std::atoi(mode.c_str() + dash + 1));
        mode = mode.substr(0, dash);
    }
    std::uint16_t bound = 0;
    const int listener = mode == "forward6" ? listenOn6(port, &bound) : listenOn(port, &bound);
    if (listener < 0) {
        return 6;
    }
    if (authLogin) mode = "helper";
    if (mode == "group") {
        spawnGrandchild(argv[0]);
    }
    std::uint16_t redirect = bound;
    if (mode == "forward" || mode == "forward6") {
        // A port free now, which the window's gate binds: the redirect as from another machine.
        std::uint16_t other = 0;
        const int probe = listenOn(0, &other);
        if (probe < 0) {
            return 6;
        }
        close(probe);
        redirect = other;
    }
    const std::string url = std::string("https://claude.com/cai/oauth/authorize?code=true&client_id=FAKE-client") +
        "&response_type=code&redirect_uri=http%3A%2F%2Flocalhost%3A" + std::to_string(redirect) +
        "%2Fcallback&scope=user%3Ainference&code_challenge=FAKEchallenge&code_challenge_method=S256&state=" + kState;
    std::printf("\x1b[2J\x1b[H\x1b[1mWelcome to the fake Claude Code\x1b[0m\r\n");
    std::fflush(stdout);
    if (mode == "print" || mode == "both") {
        std::printf("Browser didn't open? Use the url below to sign in:\r\n\r\n\x1b[2m%s\x1b[0m\r\n\r\n", url.c_str());
        std::fflush(stdout);
    }
    if (mode != "print" && mode != "silent") {
        openInBrowser(url);
    }
    pollfd p{listener, POLLIN, 0};
    if (poll(&p, 1, 60000) <= 0) {
        return 4;
    }
    const int c = accept(listener, nullptr, nullptr);
    if (c < 0) {
        return 6;
    }
    std::string request;
    char buf[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const ssize_t n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) {
            break;
        }
        request.append(buf, static_cast<std::size_t>(n));
    }
    const std::string first = request.substr(0, request.find("\r\n"));
    const std::string want = std::string("GET /callback?");
    const bool path = first.compare(0, want.size(), want) == 0;
    const bool state = first.find(std::string("state=") + kState) != std::string::npos;
    const bool code = first.find(std::string("code=") + kCode) != std::string::npos;
    const bool error = first.find("error=") != std::string::npos;
    sigset_t term;
    if (mode == "late") {
        // The parent can stop us as soon as it reads the HTTP response.
        sigemptyset(&term);
        sigaddset(&term, SIGTERM);
        sigprocmask(SIG_BLOCK, &term, nullptr);
    }
    const char* answer = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(c, answer, std::strlen(answer), 0);
    close(c);
    close(listener);
    if (!path || !state || (!code && !error)) {
        std::printf("fake claude: a wrong request ended the login\r\n");
        return 5;
    }
    if (error) {
        std::printf("\x1b[31mOAuth error: access_denied\x1b[0m\r\n");
        return 1;
    }
    if (authLogin) {
        // Simulate the identity chosen in the browser, independently of the --email hint used to open it.
        const char* email = first.find(std::string(kCode) + "Other") != std::string::npos
                          ? "other@example.test" : "work@example.test";
        std::ofstream identity(std::string(config) + "/test-browser-email");
        identity << email;
        std::ofstream grant(std::string(config) + "/.credentials.json");
        grant << R"({"claudeAiOauth":{"accessToken":"fixture-access","refreshToken":"fixture-refresh",)"
                 R"("expiresAt":4102444800000,"scopes":["user:profile","user:inference"]}})";
        return identity.good() && grant.good() ? 0 : 3;
    }
    if (mode == "late") {
        // Slow to print: the token comes only once the runner has told it to end (SIGTERM), in the grace before the
        // SIGKILL, as when the flow's deadline passed while the browser was sent back.

        int sig = 0;
        sigwait(&term, &sig);
    }
    std::printf("\x1b[32m\xe2\x9c\x93 Long-lived authentication token created successfully!\x1b[0m\r\n\r\n"
                "Your OAuth token (valid for 1 year):\r\n\r\n\x1b[33m%s\x1b[0m\r\n\r\n"
                "Store this token securely.\r\n", mode == "apikey" ? kApiKey : kToken);
    std::fflush(stdout);
    if (mode == "quick") {
        return 0;       // at once: what it printed is still in the terminal when its exit is seen
    }
    // As the real program may: a moment before it exits, its output read meanwhile.
    usleep(50000);
    return 0;
}
