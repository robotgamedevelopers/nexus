// The server's terminals for real (server/terminal_host.h, docs/terminals.md): shells on real pseudoterminals, read into
// real screens, in a directory of the test's own. Functional: real processes and real time, with short deadlines.
//
//   create   a shell starts in the directory asked for, takes input, and its screen shows the output
//   modes    a program's input modes reach the view; its answers to queries go back to it
//   resize   the pseudoterminal and the screen take a new size, which the program sees
//   exit     a shell that exits is shown ended with its code, never silently; its input is then refused
//   close    a close ends a running shell and removes its terminal; a second close is refused
//   env      the shell's environment holds no NEXUS_ variable, and TERM is xterm-256color
//   owner    a link that is not the owner's gets no shell
//
//   terminals_functional <group> <work dir>

#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "server/terminal_host.h"

extern char** environ;

namespace {

using namespace nexus;
using server::TerminalHost;
using sync::Action;
using sync::ActionKind;

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

std::string work;

server::ShellLaunch shell() {
    server::ShellLaunch s = server::loginShell(environ);
    // A plain sh with no profile, so that the test does not depend on the machine's startup files; the environment
    // and the rest are the login shell's.
    s.program = "/bin/sh";
    s.arguments = {};
    s.environment.push_back("PS1=$ ");
    s.environment.push_back("ENV=/dev/null");
    return s;
}

Action make(std::string dir = {}) {
    Action a;
    a.kind = ActionKind::TerminalCreate;
    a.text = "w1";
    a.account = std::move(dir);
    a.columns = 200;
    a.rows = 10;
    return a;
}

Action on(ActionKind kind, sync::TerminalId id, std::string input = {}) {
    Action a;
    a.kind = kind;
    a.terminal = id;
    a.secret = sync::SecretValue(input);
    return a;
}

std::string screen(const sync::TerminalView& v) {
    std::string out;
    for (const auto& l : v.lines) {
        out += *l + "\n";
    }
    return out;
}

// Each marker the tests wait for is computed by the shell ($((6*7))), so that the echo of the typed line never matches.
// Serves the host until `done` holds for the terminal's view, or 10 s pass. Returns the last view.
template <typename Done>
sync::TerminalRef until(TerminalHost& host, sync::TerminalId id, Done done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    sync::TerminalRef last;
    while (std::chrono::steady_clock::now() < end) {
        std::vector<pollfd> fds = host.fds();
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 20);
        host.serve();
        for (const auto& v : host.views()) {
            if (v->id == id) {
                last = v;
            }
        }
        if (last && done(*last)) {
            return last;
        }
    }
    std::fprintf(stderr, "timed out; the screen:\n%s", last ? screen(*last).c_str() : "(none)\n");
    return last;
}

sync::TerminalId start(TerminalHost& host, std::string dir = {}) {
    CHECK(host.apply(make(std::move(dir)), true).empty());
    CHECK(host.book().terminals().size() >= 1);
    return host.book().terminals().rbegin()->first;
}

bool shows(const sync::TerminalRef& v, const std::string& text) {
    return v && screen(*v).find(text) != std::string::npos;
}

void testCreate() {
    TerminalHost host(1, shell());
    const sync::TerminalId id = start(host, work);
    CHECK(host.apply(on(ActionKind::TerminalInput, id, "echo hi-$((40+2)); pwd\r"), true).empty());
    auto v = until(host, id, [](const sync::TerminalView& t) {
        return screen(t).find("hi-42") != std::string::npos && screen(t).find(work + "\n") != std::string::npos;
    });
    CHECK(shows(v, "hi-42"));
    CHECK(shows(v, work));      // started in the directory asked for
    CHECK(v && v->opener == "w1" && v->ended.empty() && v->columns == 200 && v->rows == 10);
    // A directory that is not one: the home directory.
    const sync::TerminalId other = start(host, work + "/no-such-dir");
    CHECK(host.apply(on(ActionKind::TerminalInput, other, "pwd\r"), true).empty());
    const char* home = std::getenv("HOME");
    v = until(host, other, [&](const sync::TerminalView& t) { return home && screen(t).find(home) != std::string::npos; });
    CHECK(home && shows(v, home));
}

void testModes() {
    TerminalHost host(1, shell());
    const sync::TerminalId id = start(host);
    // The program sets application cursor keys and bracketed paste, then asks where the cursor is: the emulator's
    // answer goes back to it, and `read` prints what it got.
    CHECK(host.apply(on(ActionKind::TerminalInput, id,
        "printf '\\033[?1h\\033[?2004h'; stty -echo raw; printf '\\033[6n'; dd bs=1 count=6 2>/dev/null | od -c | head -1;"
        " stty sane; echo done-$((6*7))\r"), true).empty());
    auto v = until(host, id, [](const sync::TerminalView& t) { return screen(t).find("done-42") != std::string::npos; });
    CHECK(v && v->modes == "1,2004");
    CHECK(shows(v, "033   ["));      // the cursor report reached the program
}

void testResize() {
    TerminalHost host(1, shell());
    const sync::TerminalId id = start(host);
    Action r = on(ActionKind::TerminalResize, id);
    r.columns = 91;
    r.rows = 17;
    CHECK(host.apply(r, true).empty());
    CHECK(host.apply(on(ActionKind::TerminalInput, id, "stty size\r"), true).empty());
    auto v = until(host, id, [](const sync::TerminalView& t) { return screen(t).find("17 91") != std::string::npos; });
    CHECK(shows(v, "17 91"));
    CHECK(v && v->columns == 91 && v->rows == 17 && v->lines.size() == 17);
}

void testExit() {
    TerminalHost host(1, shell());
    const sync::TerminalId id = start(host);
    CHECK(host.apply(on(ActionKind::TerminalInput, id, "exit 3\r"), true).empty());
    auto v = until(host, id, [](const sync::TerminalView& t) { return !t.ended.empty(); });
    CHECK(v && v->ended == "exited with code 3");
    CHECK(shows(v, "[exited with code 3]"));    // on the screen too
    const std::string refused = host.apply(on(ActionKind::TerminalInput, id, "echo no\r"), true);
    CHECK(refused.find("ended") != std::string::npos);
    // It stays until closed.
    host.serve();
    CHECK(host.views().size() == 1);
    CHECK(host.apply(on(ActionKind::TerminalClose, id), true).empty());
    CHECK(host.views().empty());
    // A shell killed by a signal says so.
    const sync::TerminalId killed = start(host);
    CHECK(host.apply(on(ActionKind::TerminalInput, killed, "kill -9 $$\r"), true).empty());
    v = until(host, killed, [](const sync::TerminalView& t) { return !t.ended.empty(); });
    CHECK(v && v->ended == "ended by signal 9");
}

void testClose() {
    TerminalHost host(1, shell());
    const sync::TerminalId id = start(host);
    CHECK(host.apply(on(ActionKind::TerminalInput, id, "echo $$ > " + work + "/pid; echo written-$((6*7))\r"), true).empty());
    auto v = until(host, id, [](const sync::TerminalView& t) { return screen(t).find("written-42") != std::string::npos; });
    CHECK(shows(v, "written-42"));
    FILE* f = std::fopen((work + "/pid").c_str(), "r");
    int pid = 0;
    CHECK(f != nullptr && std::fscanf(f, "%d", &pid) == 1);
    if (f) std::fclose(f);
    CHECK(host.apply(on(ActionKind::TerminalClose, id), true).empty());
    CHECK(host.views().empty());
    // The shell is ended and reaped within the deadline.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool gone = false;
    while (!gone && std::chrono::steady_clock::now() < end) {
        host.serve();
        gone = pid > 0 && ::kill(pid, 0) != 0;
        ::usleep(10 * 1000);
    }
    CHECK(gone);
    CHECK(host.apply(on(ActionKind::TerminalClose, id), true).find("no terminal") != std::string::npos);
    CHECK(!host.apply(on(ActionKind::TerminalInput, id, "x"), true).empty());
}

void testEnv() {
    ::setenv("NEXUS_TEST_SECRET", "must-not-reach-a-shell", 1);
    TerminalHost host(1, server::loginShell(environ));
    ::unsetenv("NEXUS_TEST_SECRET");
    const sync::TerminalId id = start(host);
    CHECK(host.apply(on(ActionKind::TerminalInput, id, "echo \"[$NEXUS_TEST_SECRET][$TERM]\"\r"), true).empty());
    auto v = until(host, id, [](const sync::TerminalView& t) {
        return screen(t).find("[][xterm-256color]") != std::string::npos;
    });
    CHECK(shows(v, "[][xterm-256color]"));
    CHECK(!shows(v, "must-not-reach"));
}

void testOwner() {
    TerminalHost host(1, shell());
    CHECK(host.apply(make(), false).find("owner") != std::string::npos);
    CHECK(host.views().empty() && host.fds().empty());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: terminals_functional <group> <work dir>\n");
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    const std::string group = argv[1];
    work = argv[2];
    ::mkdir(work.c_str(), 0700);
    char real[4096];
    if (::realpath(work.c_str(), real) != nullptr) work = real;
    if (group == "create") testCreate();
    else if (group == "modes") testModes();
    else if (group == "resize") testResize();
    else if (group == "exit") testExit();
    else if (group == "close") testClose();
    else if (group == "env") testEnv();
    else if (group == "owner") testOwner();
    else {
        std::fprintf(stderr, "unknown group %s\n", group.c_str());
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("terminals %s: ok\n", group.c_str());
    return 0;
}
