// Functional tests of in-app sign-in's processes (docs/secrets.md "Sign-in"): nexus-server's runner
// (src/server/signin_runner.h) starting the FAKE `claude` (fake_claude.cpp) under a pty, its helper
// (nexus-signin-open) handing the URL over, the window's gate (src/signin_gate.h) on loopback, and the replay. The test
// plays the browser, on loopback only. Real processes, sockets and time: functional, never unit. No real `claude`, no
// real login page, no real credential: every value is fake_claude.h's.
//
//   signin_functional <fake claude> <helper> <case>      in the directory the case may write in (its working directory)
//
// Cases:
//   same_machine   the helper hands the URL over; the browser's redirect goes straight to the program's listener
//   printed_url    the program prints its URL instead (it opened a browser by a path of its own): the scanner takes it
//   forwarded      the server on another machine: the gate takes the one callback (a stray request and a wrong state
//                  first, each answered "not found", the program untouched), the runner replays it to the program,
//                  which listens on 127.0.0.1 alone (the replay tries [::1] first, then 127.0.0.1)
//   forwarded_v6   the same, the program listening on [::1] alone
//   quick_exit     the program exits the moment its token is printed: the runner reads its terminal before closing it
//   group          the program leaves a process of its own running: stopped, or exiting, its whole group is ended
//   declined       the person declines on the page: the program exits 1, and its end is reported
//   api_key        the program prints an API key: the scanner passes it over, and the program's end is reported
//   stopped        the flow ends before any URL: the process is ended, its end not reported, its directory removed
//   unguarded      a wrong request straight to the program's listener ends the login: why the gate refuses them
//   gate_in_use    the gate's port is taken: the gate says so, and takes nothing
//   gate_v6_in_use the gate's port is free on 127.0.0.1 but held on [::1]: the gate fails, and listens on neither
//   late           the flow ends after the browser was sent back: the process, told to end, prints its token in the
//                  grace, and the runner hands it over all the same
//   crashed        a server ended without its runner's end: the next runner ends the program it left (it holds the
//                  lock in .signin) and removes every .signin directory left behind, but none under an account entry
//                  that is not itself a directory (a link)
//
// Each case also checks that the runner's log lines hold no byte of the URL's state, the code or the token, and that
// the program saw none of the test's own API key variables (the fake exits 3 if it did).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fake_claude.h"
#include "leak_scan.h"
#include "server/signin_runner.h"
#include "signin_gate.h"
#include "sync/signin.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

std::string fakeProgram;
std::string helperProgram;

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A port free now on loopback.
std::uint16_t freePort() {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    close(s);
    return ntohs(a.sin_port);
}

// The browser: one GET to loopback `port`, its answer read until the peer closes; empty when nothing answered within
// 5 s. Tries to connect for up to 5 s (a gate binds on a thread of its own).
std::string browserGet(std::uint16_t port, const std::string& target, std::uint64_t patienceMs = 5000) {
    const std::uint64_t until = nowMs() + patienceMs;
    int s = -1;
    while (nowMs() < until) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
            break;
        }
        close(s);
        s = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (s < 0) {
        return std::string();
    }
    const std::string request = "GET " + target + " HTTP/1.1\r\nHost: localhost:" + std::to_string(port) +
        "\r\nUser-Agent: fake-browser\r\n\r\n";
    send(s, request.data(), request.size(), 0);
    std::string out;
    char buf[1024];
    while (nowMs() < until) {
        pollfd p{s, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) {
            continue;
        }
        const ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    close(s);
    return out;
}

struct Run {
    std::unique_ptr<server::SignInRunner> runner;
    bool gotUrl = false;
    std::uint64_t urlId = 0;
    sync::SecretValue url;
    bool gotToken = false;
    sync::SecretValue token;
    bool exited = false;
    std::string why;
    std::vector<std::string> log;
    std::string accounts;
};

// A runner over the fake as bin/claude-<mode>, in this case's directory, opened; its events land in `r`. Relative
// paths keep the helper's socket's path short, whatever the build tree's is.
void setUp(Run& r, const std::string& mode) {
    r.accounts = "accounts";
    std::error_code ec;
    std::filesystem::remove_all("bin", ec);
    std::filesystem::create_directories("bin");
    std::filesystem::create_symlink(fakeProgram, "bin/claude-" + mode, ec);
    CHECK(!ec);
    std::filesystem::create_directories(r.accounts);
    server::SignInRunner::Options o;
    o.claude = "bin/claude-" + mode;
    o.helper = helperProgram;
    o.accounts = r.accounts;
    o.dir = ".";
    o.home = std::filesystem::current_path().string();
    o.path = "/usr/bin:/bin";
    o.now = nowMs;
    if (mode.rfind("forward", 0) == 0 && mode.find('-') != std::string::npos) {
        o.replayPort = static_cast<std::uint16_t>(std::atoi(mode.c_str() + mode.find('-') + 1));
    }
    server::SignInRunner::Events e;
    e.url = [&r](const std::string&, std::uint64_t id, const sync::SecretValue& url) {
        r.gotUrl = true;
        r.urlId = id;
        r.url = url;
    };
    e.printed = [&r](const std::string&, const sync::SecretValue& token) {
        r.gotToken = true;
        r.token = token;
    };
    e.exited = [&r](const std::string&, std::uint64_t, const std::string& why) {
        r.exited = true;
        r.why = why;
    };
    e.log = [&r](const std::string& line) { r.log.push_back(line); };
    r.runner = std::make_unique<server::SignInRunner>(std::move(o), std::move(e));
    std::string why;
    CHECK(r.runner->open(&why));
}

// Serves the runner until `done` or 20 s.
bool pump(Run& r, const std::function<bool()>& done) {
    const std::uint64_t until = nowMs() + 20000;
    while (!done() && nowMs() < until) {
        std::vector<pollfd> fds = r.runner->fds();
        poll(fds.data(), static_cast<nfds_t>(fds.size()), 50);
        r.runner->serve();
    }
    return done();
}

// The runner's log lines hold nothing of the values, and the account's directory keeps no config directory.
void checkClean(const Run& r) {
    const test::LeakScan scans[] = {test::LeakScan(fake_claude::kState), test::LeakScan(fake_claude::kCode),
                                    test::LeakScan(fake_claude::kToken), test::LeakScan(fake_claude::kApiKey)};
    for (const std::string& line : r.log) {
        for (const test::LeakScan& s : scans) {
            const std::string found = s.find(line);
            if (!found.empty()) {
                std::fprintf(stderr, "a log line gives a value away: %s\n", found.c_str());
                ++failures;
            }
        }
    }
    CHECK(!std::filesystem::exists(r.accounts + "/claude-main/.signin"));
}

std::string callback(const std::string& state, const std::string& rest) {
    return "/callback?" + rest + "&state=" + state;
}

// Starts the flow and waits for its URL; true with the URL's target.
bool startAndUrl(Run& r, sync::SignInTarget* target) {
    r.runner->start(7, "claude-main");
    CHECK(r.runner->running("claude-main"));
    const bool got = pump(r, [&] { return r.gotUrl || r.exited; });
    CHECK(got && r.gotUrl);
    CHECK(r.urlId == 7);
    return r.gotUrl && sync::parseSignInUrl("claude", r.url.bytes(), target);
}

void sameMachine(const std::string& mode) {
    Run r;
    setUp(r, mode);
    sync::SignInTarget t;
    CHECK(startAndUrl(r, &t));
    CHECK(t.state == fake_claude::kState);
    CHECK(t.path == "/callback");
    const std::string answer = browserGet(t.port, callback(t.state, std::string("code=") + fake_claude::kCode));
    CHECK(answer.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(pump(r, [&] { return r.gotToken; }));
    CHECK(r.token.bytes() == fake_claude::kToken);
    CHECK(sync::isSubscriptionToken(r.token));
    CHECK(pump(r, [&] { return !r.runner->running("claude-main"); }));
    CHECK(!r.exited);   // a flow that printed its token ends as captured, not as an exit
    checkClean(r);
}

void caseSameMachine() { sameMachine("helper"); }
void casePrintedUrl() { sameMachine("print"); }

// Whether this machine has an IPv6 loopback to listen on.
bool hasIpv6Loopback() {
    const int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0) {
        return false;
    }
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    const bool ok = bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    close(s);
    return ok;
}

// The server on another machine (`mode` forward: the program listens on 127.0.0.1 alone; forward6: on [::1] alone):
// the gate takes the one callback, and the runner replays it to the redirect's localhost, IPv6 loopback first, then
// IPv4, so that it reaches the program whichever it listens on.
void forwarded(const std::string& family) {
    const std::uint16_t listener = freePort();
    const std::string mode = family + "-" + std::to_string(listener);
    Run r;
    setUp(r, mode);
    sync::SignInTarget t;
    CHECK(startAndUrl(r, &t));
    CHECK(t.port != listener);
    SignInGate gate(t);
    sync::SecretValue query;
    // A stray request and a wrong state: each "not found", and nothing taken.
    CHECK(browserGet(t.port, "/favicon.ico").rfind("HTTP/1.1 404", 0) == 0);
    CHECK(browserGet(t.port, callback("fakeWrongStateXq", std::string("code=") + fake_claude::kCode))
              .rfind("HTTP/1.1 404", 0) == 0);
    CHECK(!gate.take(&query));
    const std::string answer = browserGet(t.port, callback(t.state, std::string("code=") + fake_claude::kCode));
    CHECK(answer.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(answer.find("signed in, you can close this tab") != std::string::npos);
    CHECK(answer.find(fake_claude::kCode) == std::string::npos);
    const std::uint64_t until = nowMs() + 5000;
    bool took = false;
    while (!took && nowMs() < until) {
        took = gate.take(&query);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(took);
    CHECK(sync::queryValue(query.bytes(), "code") == fake_claude::kCode);
    // The gate takes one: a second, right callback finds it closed (given the gate's poll to notice), or taken.
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * SignInGate::kPollMs));
    const std::string again = browserGet(t.port, callback(t.state, std::string("code=") + fake_claude::kCode), 300);
    CHECK(again.empty() || again.rfind("HTTP/1.1 404", 0) == 0);
    sync::SecretValue second;
    CHECK(!gate.take(&second));
    r.runner->replay(r.urlId, "claude-main", t, query);
    query.clear();
    CHECK(pump(r, [&] { return r.gotToken || r.exited; }));
    CHECK(r.gotToken && r.token.bytes() == fake_claude::kToken);
    CHECK(pump(r, [&] { return !r.runner->running("claude-main"); }));
    CHECK(!r.exited);
    for (const std::string& line : r.log) {
        CHECK(line.find("not replayed") == std::string::npos);
    }
    checkClean(r);
}

void caseForwarded() { forwarded("forward"); }

void caseForwardedV6() {
    if (!hasIpv6Loopback()) {
        std::printf("signin_functional forwarded_v6: this machine has no IPv6 loopback; nothing to check\n");
        return;
    }
    forwarded("forward6");
}

// The program prints its token and exits at once: what it printed is read from its terminal before the terminal is
// closed (the runner drains it after it sees the exit), so the token is handed over.
void caseQuickExit() {
    for (int i = 0; i < 5; ++i) {
        Run r;
        setUp(r, "quick");
        sync::SignInTarget t;
        CHECK(startAndUrl(r, &t));
        browserGet(t.port, callback(t.state, std::string("code=") + fake_claude::kCode));
        CHECK(pump(r, [&] { return !r.runner->running("claude-main"); }));
        CHECK(r.gotToken && r.token.bytes() == fake_claude::kToken && !r.exited);
        checkClean(r);
    }
}

// The pid the `group` fake wrote, or -1.
pid_t grandchildPid() {
    FILE* f = std::fopen("bin/grandchild.pid", "r");
    if (f == nullptr) {
        return -1;
    }
    long v = -1;
    if (std::fscanf(f, "%ld", &v) != 1) {
        v = -1;
    }
    std::fclose(f);
    return static_cast<pid_t>(v);
}

// Whether `pid` has ended (and been collected by init) within 5 s.
bool endsSoon(pid_t pid) {
    const std::uint64_t until = nowMs() + 5000;
    while (nowMs() < until) {
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// A program that left a process of its own running (in its group, deaf to the hangup): stopped, its whole group is
// ended; exiting after its token, its group is ended as it is collected. No process of a sign-in outlives it.
void caseGroup() {
    {
        Run r;
        setUp(r, "group");
        sync::SignInTarget t;
        CHECK(startAndUrl(r, &t));
        const pid_t g = grandchildPid();
        CHECK(g > 1 && kill(g, 0) == 0);
        r.runner->stop("claude-main");
        CHECK(pump(r, [&] { return !r.runner->running("claude-main"); }));
        CHECK(g > 1 && endsSoon(g));
        checkClean(r);
    }
    {
        Run r;
        setUp(r, "group");
        sync::SignInTarget t;
        CHECK(startAndUrl(r, &t));
        const pid_t g = grandchildPid();
        CHECK(g > 1 && kill(g, 0) == 0);
        browserGet(t.port, callback(t.state, std::string("code=") + fake_claude::kCode));
        CHECK(pump(r, [&] { return !r.runner->running("claude-main"); }));
        CHECK(r.gotToken && r.token.bytes() == fake_claude::kToken);
        CHECK(g > 1 && endsSoon(g));
        checkClean(r);
    }
}

void caseDeclined() {
    Run r;
    setUp(r, "deny");
    sync::SignInTarget t;
    CHECK(startAndUrl(r, &t));
    browserGet(t.port, callback(t.state, "error=access_denied"));
    CHECK(pump(r, [&] { return r.exited; }));
    CHECK(r.why == "it exited 1");
    CHECK(!r.gotToken);
    checkClean(r);
}

void caseApiKey() {
    Run r;
    setUp(r, "apikey");
    sync::SignInTarget t;
    CHECK(startAndUrl(r, &t));
    browserGet(t.port, callback(t.state, std::string("code=") + fake_claude::kCode));
    // Not a subscription's login: the scanner passes it over, so nothing is printed; the program's end is reported
    // (the flow fails).
    CHECK(pump(r, [&] { return r.exited; }));
    CHECK(!r.gotToken && r.why == "it exited 0");
    checkClean(r);
}

void caseStopped() {
    Run r;
    setUp(r, "silent");
    r.runner->start(7, "claude-main");
    CHECK(r.runner->running("claude-main"));
    CHECK(std::filesystem::exists(r.accounts + "/claude-main/.signin"));
    r.runner->stop("claude-main");
    CHECK(pump(r, [&] { return !r.runner->running("claude-main"); }));
    CHECK(!r.exited);   // the flow ended first: its process's end is not news
    CHECK(!r.gotUrl);
    checkClean(r);
}

// The flow ends (its deadline, say) after the browser was sent back but before the token: the process, told to end,
// prints its token in the grace before the SIGKILL, and the runner still hands it over (the hub stores it late).
void caseLate() {
    Run r;
    setUp(r, "late");
    sync::SignInTarget t;
    CHECK(startAndUrl(r, &t));
    const std::string answer = browserGet(t.port, callback(t.state, std::string("code=") + fake_claude::kCode));
    CHECK(answer.rfind("HTTP/1.1 200", 0) == 0);
    r.runner->stop("claude-main");
    CHECK(pump(r, [&] { return r.gotToken; }));
    CHECK(r.token.bytes() == fake_claude::kToken && sync::isSubscriptionToken(r.token));
    CHECK(pump(r, [&] { return !r.runner->running("claude-main"); }));
    CHECK(!r.exited);
    checkClean(r);
}

void caseUnguarded() {
    Run r;
    setUp(r, "helper");
    sync::SignInTarget t;
    CHECK(startAndUrl(r, &t));
    browserGet(t.port, "/favicon.ico");
    CHECK(pump(r, [&] { return r.exited; }));
    CHECK(r.why == "it exited 5");
    checkClean(r);
}

void caseGateInUse() {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    CHECK(bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 && listen(s, 1) == 0);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    sync::SignInTarget t;
    t.port = ntohs(a.sin_port);
    t.path = "/callback";
    t.state = fake_claude::kState;
    SignInGate gate(t);
    std::string why;
    const std::uint64_t until = nowMs() + 5000;
    bool failed = false;
    while (!failed && nowMs() < until) {
        failed = gate.failed(&why);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(failed);
    CHECK(why.find("cannot be received on this machine") != std::string::npos);
    sync::SecretValue query;
    CHECK(!gate.take(&query));
    close(s);
}

// The port free on 127.0.0.1 but held on [::1] by another program: the gate fails, with a plain line, rather than
// listen on 127.0.0.1 alone while a browser that tries ::1 first gives the callback to that program.
void caseGateV6InUse() {
    const int s = socket(AF_INET6, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    socklen_t len = sizeof a;
    if (s < 0 || bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || listen(s, 1) != 0) {
        std::printf("signin_functional gate_v6_in_use: this machine has no IPv6 loopback; nothing to check\n");
        if (s >= 0) {
            close(s);
        }
        return;
    }
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    sync::SignInTarget t;
    t.port = ntohs(a.sin6_port);
    t.path = "/callback";
    t.state = fake_claude::kState;
    SignInGate gate(t);
    std::string why;
    const std::uint64_t until = nowMs() + 5000;
    bool failed = false;
    while (!failed && nowMs() < until) {
        failed = gate.failed(&why);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(failed);
    CHECK(why.find("cannot be received on this machine: [::1]") != std::string::npos);
    // Nothing listens on 127.0.0.1 for it either: the gate let go of both.
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * SignInGate::kPollMs));
    CHECK(browserGet(t.port, "/callback", 300).empty());
    close(s);
}

// The pid in a sign-in's .signin/pid, or -1.
pid_t recordedPid(const std::string& configDir) {
    FILE* f = std::fopen((configDir + "/pid").c_str(), "r");
    if (f == nullptr) {
        return -1;
    }
    long v = -1;
    if (std::fscanf(f, "%ld", &v) != 1) {
        v = -1;
    }
    std::fclose(f);
    return static_cast<pid_t>(v);
}

// A server that ended without its runner's end (a crash): a child process starts a sign-in and _exits, leaving its
// program running and its .signin behind. The next runner's open() ends that program and removes the directory; a
// .signin no program holds (another account's) is removed as well; a .signin that is not a directory is left alone.
void caseCrashed() {
    std::error_code ec;
    std::filesystem::remove_all("accounts", ec);
    std::filesystem::create_directories("accounts/claude-odd");
    std::FILE* odd = std::fopen("accounts/claude-odd/.signin", "w");
    CHECK(odd != nullptr);
    if (odd != nullptr) {
        std::fclose(odd);
    }
    // An account entry that is a link to a directory elsewhere (never ours to sweep): its .signin is left alone.
    std::filesystem::remove_all("elsewhere", ec);
    std::filesystem::create_directories("elsewhere/.signin");
    std::filesystem::create_directory_symlink(std::filesystem::absolute("elsewhere"), "accounts/claude-link", ec);
    CHECK(!ec);
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        Run r;
        setUp(r, "silent");
        r.runner->start(7, "claude-main");
        // Wait until the fake program has checked its terminal and written its greeting: on Linux, closing the pty
        // master in _exit() hangs up the slave (hung_up_tty_ioctl -> EIO), so isatty(0) returns 0 if called after.
        std::vector<pollfd> fds = r.runner->fds();
        bool spoke = false;
        const std::uint64_t readyUntil = nowMs() + 5000;
        while (fds.size() >= 2 && !spoke && nowMs() < readyUntil) {
            fds[1].revents = 0;
            if (poll(&fds[1], 1, 50) > 0 && (fds[1].revents & POLLIN) != 0) {
                spoke = true;
            }
        }
        _exit(spoke && r.runner->running("claude-main") && failures == 0 ? 0 : 1);    // no destructor: as a crash leaves it
    }
    CHECK(child > 0);
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    const pid_t left = recordedPid("accounts/claude-main/.signin");
    CHECK(left > 1);
    CHECK(left > 1 && kill(left, 0) == 0);      // the program outlived its server
    // Another account's, from a crash longer ago: its program gone, nothing holds its lock.
    std::filesystem::create_directories("accounts/claude-other/.signin");
    std::FILE* stale = std::fopen("accounts/claude-other/.signin/lock", "w");
    CHECK(stale != nullptr);
    if (stale != nullptr) {
        std::fclose(stale);
    }

    Run r;
    setUp(r, "silent");                         // open() sweeps first
    bool gone = false;
    const std::uint64_t until = nowMs() + 5000;
    while (left > 1 && !gone && nowMs() < until) {
        gone = kill(left, 0) != 0 && errno == ESRCH;   // ended, and reaped by init
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(gone);
    CHECK(!std::filesystem::exists("accounts/claude-main/.signin"));
    CHECK(!std::filesystem::exists("accounts/claude-other/.signin"));
    CHECK(std::filesystem::is_regular_file("accounts/claude-odd/.signin"));
    CHECK(std::filesystem::is_directory("elsewhere/.signin"));
    int ended = 0;
    int removed = 0;
    for (const std::string& line : r.log) {
        if (line.find("claude-main's sign-in left by an earlier server: its process ended") != std::string::npos) {
            ++ended;
        }
        if (line == "sign-in: claude-other's sign-in left by an earlier server: its directory removed") {
            ++removed;
        }
    }
    CHECK(ended == 1 && removed == 1);
    if (ended != 1 || removed != 1) {
        for (const std::string& line : r.log) {
            std::fprintf(stderr, "  log: %s\n", line.c_str());   // the sweep's lines: account names only
        }
    }
    checkClean(r);
}

struct Case {
    const char* name;
    void (*run)();
};

const Case kCases[] = {
    {"same_machine", caseSameMachine}, {"printed_url", casePrintedUrl}, {"forwarded", caseForwarded},
    {"declined", caseDeclined},        {"api_key", caseApiKey},         {"stopped", caseStopped},
    {"unguarded", caseUnguarded},      {"gate_in_use", caseGateInUse},  {"late", caseLate},
    {"gate_v6_in_use", caseGateV6InUse}, {"crashed", caseCrashed}, {"forwarded_v6", caseForwardedV6},
    {"quick_exit", caseQuickExit},     {"group", caseGroup},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: signin_functional <fake claude> <helper> <case>\n");
        return 2;
    }
    fakeProgram = argv[1];
    helperProgram = argv[2];
    // Made-up key variables in this process's environment: the runner must pass none of them to the program.
    setenv("ANTHROPIC_API_KEY", "FAKEnotAKeyForTheChild", 1);
    setenv("CLAUDE_CODE_OAUTH_TOKEN", "FAKEnotATokenForTheChild", 1);
    for (const Case& c : kCases) {
        if (std::strcmp(argv[3], c.name) == 0) {
            c.run();
            if (failures != 0) {
                std::fprintf(stderr, "signin_functional %s: %d check(s) failed\n", c.name, failures);
                return 1;
            }
            std::printf("signin_functional %s: ok\n", c.name);
            return 0;
        }
    }
    std::fprintf(stderr, "signin_functional: no case %s\n", argv[3]);
    return 2;
}
