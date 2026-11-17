// Functional tests of the link, end to end: nexus-server started over real ssh to this machine by the window's own
// bootstrap, and headless windows (the window's model and mirror, without drawing) talking to it over real UDP through
// mosh's transport. Each run has a server of its own, in the directory <dir> it is given (its socket and log), and
// stops it at the end. A run stops any server an earlier run left in <dir> first, and the `stop` form (the tests'
// cleanup fixture, which ctest runs even after a crash or a timeout) stops the servers in every directory it names, so
// no server outlives the tests. The session key is never printed.
//
//   link_e2e <nexus-server> <dir> roundtrip <host> <nexus-fake-driver>
//       the window's own link thread (link/client.h) through ssh <host>: a prompt sent to one friend comes back with
//       its reply from the friend's driver (the fake, named in <dir>/drivers), one queued to another shows queued;
//       then the same from a window that states no state format (one from before formats), read in format 1;
//       then the link shuts down and the server ends the session
//   link_e2e <nexus-server> <dir> outage
//       through a relay on loopback that the test can cut: prompts sent while the network is down for 8 s arrive once
//       each when it comes back, the window says it has no contact while it is down, and converges after
//   link_e2e <nexus-server> <dir> soak <clients> <prompts>
//       several windows at once, prompts from each to random friends: every window ends holding the server's state,
//       each prompt applied once, each window's prompts in its order, and every window's transcripts agree
//   link_e2e <nexus-server> <dir> restart
//       the server started by the test and killed with SIGKILL while a window is connected, then started again: a new
//       session of the same window (its client id) finds every session and queue there, sends all its actions again
//       under the same numbers, and only the new one is applied; a session that would skip actions is refused
//   link_e2e <nexus-server> <dir> flush <nexus-fake-driver>
//       the server started by the test with its store's flush gated (server/flush_gate.h): a batch held between its
//       write and its flush tells the driver nothing, shows the window nothing and acknowledges nothing; lost there (the
//       server killed), the restarted server holds what was durable, and the window's action sent again is applied
//       and sent to the driver once
//   link_e2e <nexus-server> <dir> secret <nexus-fake-driver>
//       the server started by the test under a HOME of its own in <dir>: ada's driver asks for a1's login, the window
//       answers the request it sees with a fake value over the real link, the value lands in its file (0600) and,
//       the server stopped, no run, hex, base64 or length of it is in any other file the run left (server.log with
//       the server's and the driver's standard error, the driver's environment, the store) or the window's state
//   link_e2e <nexus-server> <dir> friends <nexus-fake-driver>
//       a fresh server under a HOME of its own in <dir> with one friend, ada, on the fake driver: a window that knows no
//       friend shows ada's tab alone, from the server's list, and a prompt typed there starts ada's session and comes
//       back answered; a friend added to friends.json and reloaded appears in the open window, and goes once taken out
//   link_e2e <nexus-server> <dir> signin <fake claude> [leaky]
//       in-app sign-in through the real server with the FAKE claude and two windows: which link gets the URL, the
//       flow let go of when its window's link ends and taken over by the other, the forwarded callback replayed, the
//       token in a1's file, `config sign-in`, and every file and the other window's states searched for the values;
//       `leaky`, with nexus-server-leaky: the search must find the leak that build writes to server.log
//   link_e2e <nexus-server> stop <dir>...
//       stops the server in each directory, if one runs, and removes the directory

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fake_mosh.h"
#include "command.h"
#include "leak_scan.h"
#include "link/bootstrap.h"
#include "link/client.h"
#include "link/mosh_link.h"
#include "model.h"
#include "sync/client.h"
#include "sync/friend_strip.h"
#include "sync/hub.h"
#include "sync/mirror.h"

extern char** environ;

namespace {

using namespace nexus;
using Clock = std::chrono::steady_clock;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

const std::vector<FriendInfo> kFriends = {{"ada", "m1"}, {"bo", "m2"}, {"cy", "m3"}};

// Each window first sends an ensure for every friend, in the friends' order, as the app does: the server makes a
// session for each friend that has none, minting ids from 1, so on a fresh server ada's is 1, bo's 2 and cy's 3,
// whichever window's ensure arrives first (each window's arrive in order). The runs check it against the state.
sync::SessionId sid(const std::string& name) {
    for (std::size_t i = 0; i < kFriends.size(); ++i) {
        if (kFriends[i].name == name) {
            return i + 1;
        }
    }
    std::fprintf(stderr, "link_e2e: no friend %s\n", name.c_str());
    std::exit(1);
}

sync::Action act(sync::ActionKind kind, sync::SessionId session, std::string text) {
    sync::Action a;
    a.kind = kind;
    a.session = session;
    a.text = std::move(text);
    return a;
}

std::vector<sync::Action> ensures() {
    std::vector<sync::Action> out;
    for (const FriendInfo& f : kFriends) {
        sync::Action a;
        a.kind = sync::ActionKind::Ensure;
        a.friendName = f.name;
        out.push_back(a);
    }
    return out;
}

// Whether the state holds each friend's session at the id sid() gives it, and no other.
bool sessionsAsMinted(const sync::Sessions& state) {
    for (const FriendInfo& f : kFriends) {
        const sync::SessionView* v = state.first(f.name);
        if (v == nullptr || v->id != sid(f.name)) {
            return false;
        }
    }
    return state.sessions().size() == kFriends.size();
}

// Stops the server in `dir`, if one runs there, and removes `dir`, with the server's log printed first when there is
// one. No socket, or a socket no server answers on (left by a server that was killed), is nothing to stop. False only
// when a server answered and did not stop.
bool stopIn(const std::string& binary, const std::string& dir) {
    bool ok = true;
    if (access((dir + "/server.sock").c_str(), F_OK) == 0) {
        Command c{binary, {"stop", "--dir", dir}, binary + " stop --dir " + dir};
        const CommandResult r = runCommand(c, 10);
        const bool stopped = r.end == CommandResult::End::Exited && r.code == 0;
        ok = stopped || r.err.find("no server answers") != std::string::npos;
        if (!stopped)
            std::fprintf(stderr, "link_e2e: stopping the server in %s: %s\n", dir.c_str(), r.err.c_str());
    }
    if (access((dir + "/server.log").c_str(), R_OK) == 0) {
        Command cat{"/bin/cat", {dir + "/server.log"}, "cat"};
        std::printf("--- server log, %s ---\n%s------------------\n", dir.c_str(), runCommand(cat, 5).out.c_str());
    }
    Command rm{"/bin/rm", {"-rf", dir}, "rm"};
    runCommand(rm, 5);
    return ok;
}

// The server of this run, in `dir`: whatever an earlier run left there is stopped first, and this run's server is
// stopped (and `dir` removed) at the end.
struct Server {
    std::string binary;
    std::string dir;
    Server(std::string b, std::string d) : binary(std::move(b)), dir(std::move(d)) {
        if (!stopIn(binary, dir))
            std::exit(1);
    }
    ~Server() { stopIn(binary, dir); }
    link::Bootstrap bootstrap(const std::string& host) const {
        link::Bootstrap b;
        b.host = host;
        // Its HOME is the test's own, under `dir`, so no run reads or writes the machine's ~/.nexus.
        b.command = {"/usr/bin/env", "HOME=" + dir + "/home", binary, "new", "--dir", dir};
        return b;
    }
};

// These tests check what the link does, never how fast this machine is. A wait for the link to get somewhere ends when
// its condition holds, and fails only when nothing has moved for kStallSeconds. Every action the server applies is
// durable before it is acknowledged: each pass of its loop writes the pass's actions in one write and makes them
// durable in one F_FULLFSYNC (fsync off macOS; server/server.cpp, store/journal.cpp), and that flush waits on every
// other writer to the drive, so on a loaded machine one pass can take seconds where it takes milliseconds alone. A
// fixed deadline on a whole wait measures the machine; a run that is still making progress is not cut. A link that
// hangs, or loses an action, makes no progress and still fails, kStallSeconds after its last step. kStallSeconds is
// the bound the link gives one step of its own: the bootstrap's ssh (link::Bootstrap::timeoutSeconds).
constexpr int kStallSeconds = 30;

// Calls step() until done() holds, or until progress() has not changed for kStallSeconds.
bool whileProgressing(const std::function<bool()>& done, const std::function<std::uint64_t()>& progress,
                      const std::function<void()>& step) {
    std::uint64_t last = progress();
    auto moved = Clock::now();
    while (!done()) {
        step();
        const std::uint64_t now = progress();
        if (now != last) {
            last = now;
            moved = Clock::now();
        } else if (Clock::now() - moved > std::chrono::seconds(kStallSeconds)) {
            return done();
        }
    }
    return true;
}

// One step with nothing to watch but the clock (a server starting, a link connecting): up to `seconds`.
bool waitFor(const std::function<bool()>& done, int seconds) {
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

std::size_t tabOf(const Model& m, const std::string& name) {
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        if (m.tab(i).name == name)
            return i;
    }
    std::fprintf(stderr, "link_e2e: no tab %s\n", name.c_str());
    std::exit(1);
}

double seconds(Clock::time_point since) {
    return std::chrono::duration<double>(Clock::now() - since).count();
}

// A session with no driver: the server's notice for a prompt it could not send (server/driver_table.h), which the runs
// other than roundtrip get, having no drivers file.
bool noDriver(const std::string& text) {
    return text.find(" has no driver, so the prompt was not sent") != std::string::npos;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
    return static_cast<bool>(out.flush());
}

// ---- roundtrip: the window's own link thread ----

int roundtrip(const std::string& binary, const std::string& dir, const std::string& host, const std::string& fake) {
    Server server(binary, dir);
    // ada's driver is the fake, answering one prompt; the server reads the drivers file at the prompt.
    CHECK(::mkdir(dir.c_str(), 0700) == 0);
    const std::string reply = "send\nemit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"hello \"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"back\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"tool\",\"name\":\"Read\",\"summary\":\"a file\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n";
    CHECK(writeFile(dir + "/ada.script", "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\",\"features\":[\"limit\"]}\n"
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n" + reply + reply));
    CHECK(writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script\nfriend ada fake " + dir +
        "\n"));
    // A window of this build, then one that states no state format, as a window from before formats: the server
    // answers it in format 1, which carries no activity and no tool line, so it reads the tool line as the friend's
    // text, as such a window always has.
    for (const std::uint32_t format : {sync::kStateFormat, std::uint32_t(0)}) {
        const bool older = format == 0;
        Model model(kFriends, true);
        sync::Mirror mirror;
        const auto start = Clock::now();
        link::ClientLink::Options options;
        options.bootstrap = server.bootstrap(host);
        options.bootstrap.stateFormat = format;
        link::ClientLink link(options);
        link::ClientLink::Snapshot s;
        auto pump = [&] {
            if (link.poll(&s))
                mirror.apply(model, s.sessions);
        };
        // Until the link gets somewhere: the server's next state, or its acknowledging everything sent.
        auto until = [&](const std::function<bool()>& done) {
            return whileProgressing([&] { pump(); return done(); },
                [&] { return s.stateNum * 2 + (s.acknowledged ? 1 : 0); },
                [] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
        };
        // The link ends Connecting by itself, connected or failed, within its own bounds: ssh -G (10 s), then the
        // bootstrap's ssh (timeoutSeconds). The wait is those, and one stall more.
        CHECK(waitFor([&] { pump(); return s.phase != sync::LinkPhase::Connecting; },
            10 + options.bootstrap.timeoutSeconds + kStallSeconds));
        if (s.phase == sync::LinkPhase::Failed)
            std::fprintf(stderr, "link_e2e: %s\n", s.problem.c_str());
        CHECK(s.phase == sync::LinkPhase::Connected);
        std::printf("%s window connected to %s in %.2f s\n", older ? "an older" : "this build's", host.c_str(),
            seconds(start));
        const auto sent = Clock::now();
        for (const sync::Action& a : ensures()) {
            link.send(a);
        }
        link.send(act(sync::ActionKind::Send, sid("ada"), older ? "hello from an older window" : "hello over mosh"));
        if (!older) {
            link.send(act(sync::ActionKind::Queue, sid("bo"), "for later"));
        }
        const std::size_t ada = tabOf(model, "ada");
        const std::size_t bo = tabOf(model, "bo");
        const std::size_t want = older ? 6 : 3;
        CHECK(until([&] { return model.tab(ada).transcript.size() == want && model.tab(bo).queue.size() == 1; }));
        std::printf("round trip in %.3f s\n", seconds(sent));
        const std::vector<Entry>& t = model.tab(ada).transcript;
        CHECK(t.size() == want);
        if (t.size() == want) {
            CHECK(t[want - 3].kind == EntryKind::Person);
            CHECK(t[want - 3].text == (older ? "hello from an older window" : "hello over mosh"));
            CHECK(t[want - 2].kind == EntryKind::Friend && t[want - 2].text == "hello back");
            CHECK(t[want - 1].kind == (older ? EntryKind::Friend : EntryKind::Tool));
            CHECK(t[want - 1].text == "Read: a file");
        }
        // The friend is idle once its turn ends; a window of format 1 hears no activity, and reads idle.
        const std::size_t adaId = model.tab(ada).id;
        CHECK(until([&] { const sync::Activity* a = mirror.activity(adaId); return a != nullptr &&
            *a == sync::Activity::Idle; }));
        CHECK(model.tab(bo).queue == std::vector<std::string>{"for later"});
        CHECK(model.tab(bo).transcript.empty());
        CHECK(until([&] { return s.acknowledged && s.sent == (older ? 4u : 5u); }));
        CHECK(sessionsAsMinted(s.sessions));
    }   // each link shuts down at the end of its pass
    std::printf("roundtrip through %s: %s\n", host.c_str(), failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}

// ---- the relay the outage cuts ----

class Relay {
public:
    Relay(const std::string& serverPort) {
        front_ = socket(AF_INET, SOCK_DGRAM, 0);
        back_ = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(front_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        getsockname(front_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = std::to_string(ntohs(a.sin_port));
        server_.sin_family = AF_INET;
        server_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        server_.sin_port = htons(static_cast<std::uint16_t>(std::stoi(serverPort)));
        fcntl(front_, F_SETFL, O_NONBLOCK);
        fcntl(back_, F_SETFL, O_NONBLOCK);
    }
    ~Relay() {
        close(front_);
        close(back_);
    }
    const std::string& port() const { return port_; }
    std::vector<int> fds() const { return {front_, back_}; }
    bool down = false;
    std::size_t dropped = 0;
    // Moves every datagram waiting, either way, unless the network is down.
    void pump() {
        char buf[2048];
        for (;;) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const ssize_t n = recvfrom(front_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (n < 0)
                break;
            client_ = from;
            haveClient_ = true;
            if (down)
                ++dropped;
            else
                sendto(back_, buf, static_cast<std::size_t>(n), 0, reinterpret_cast<const sockaddr*>(&server_),
                    sizeof server_);
        }
        for (;;) {
            const ssize_t n = recv(back_, buf, sizeof buf, 0);
            if (n < 0)
                break;
            if (down || !haveClient_)
                ++dropped;
            else
                sendto(front_, buf, static_cast<std::size_t>(n), 0, reinterpret_cast<const sockaddr*>(&client_),
                    sizeof client_);
        }
    }

private:
    int front_ = -1;
    int back_ = -1;
    std::string port_;
    sockaddr_in server_{};
    sockaddr_in client_{};
    bool haveClient_ = false;
};

// One headless window over a ClientSession of its own, driven by the test's loop.
struct Window {
    std::unique_ptr<link::ClientSession> session;
    Model model{kFriends, true};
    sync::Mirror mirror;
    std::uint64_t mirroredNum = ~std::uint64_t(0);
    void after() {
        if (session->stateNum() != mirroredNum) {
            mirroredNum = session->stateNum();
            mirror.apply(model, session->state());
        }
    }
};

// One round for every window (and the relay): tick, poll for up to `ms`, read.
void step(std::vector<Window*>& windows, Relay* relay, int ms) {
    link::freezeClock();
    std::string why;
    std::vector<pollfd> fds;
    std::vector<int> owner;
    int wait = ms;
    for (std::size_t w = 0; w < windows.size(); ++w) {
        windows[w]->session->tick(&why);
        wait = std::min(wait, std::max(0, windows[w]->session->waitMs()));
        for (int fd : windows[w]->session->fds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
            owner.push_back(static_cast<int>(w));
        }
    }
    if (relay != nullptr) {
        for (int fd : relay->fds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
            owner.push_back(-1);
        }
    }
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), wait);
    link::freezeClock();
    if (relay != nullptr)
        relay->pump();
    for (std::size_t k = 0; k < fds.size(); ++k) {
        if (owner[k] >= 0 && (fds[k].revents & POLLIN))
            windows[static_cast<std::size_t>(owner[k])]->session->receive(&why);
    }
    for (Window* w : windows)
        w->after();
}

bool openWindow(const Server& server, const std::string& host, const std::string& ip, const Relay* relayFor,
                Window* w, link::Connect* connect) {
    std::string why;
    if (!link::bootstrap(server.bootstrap(host), runCommand, connect, &why)) {
        std::fprintf(stderr, "link_e2e: %s\n", why.c_str());
        return false;
    }
    w->session = std::make_unique<link::ClientSession>(connect->key, ip, relayFor ? relayFor->port() : connect->port);
    return true;
}

// ---- outage ----

int outage(const std::string& binary, const std::string& dir) {
    Server server(binary, dir);
    link::Connect connect;
    std::string why;
    if (!link::bootstrap(server.bootstrap("localhost"), runCommand, &connect, &why)) {
        std::fprintf(stderr, "link_e2e: %s\n", why.c_str());
        return 1;
    }
    Relay relay(connect.port);
    Window w;
    w.session = std::make_unique<link::ClientSession>(connect.key, "127.0.0.1", relay.port());
    std::vector<Window*> windows{&w};
    const std::size_t ada = tabOf(w.model, "ada");
    auto settle = [&](const std::function<bool()>& done) {
        return whileProgressing(done, [&] { return w.session->stateNum() * 2 + (w.session->allAcknowledged() ? 1 : 0); },
            [&] { step(windows, &relay, 50); });
    };
    CHECK(settle([&] { return w.session->stateNum() > 0; }));
    for (const sync::Action& a : ensures()) {
        w.session->add(a);
    }
    w.session->add(act(sync::ActionKind::Send, sid("ada"), "before"));
    CHECK(settle([&] { return w.model.tab(ada).transcript.size() == 2 && w.session->allAcknowledged(); }));

    // The network goes down for 8 s; three prompts are sent during it.
    relay.down = true;
    const auto cut = Clock::now();
    std::uint64_t longestQuiet = 0;
    int sentDuring = 0;
    while (seconds(cut) < 8) {
        step(windows, &relay, 50);
        longestQuiet = std::max(longestQuiet, w.session->heardMsAgo());
        if (sentDuring < 3 && seconds(cut) > 1 + sentDuring) {
            w.session->add(act(sync::ActionKind::Send, sid("ada"), "during " + std::to_string(sentDuring)));
            ++sentDuring;
        }
    }
    CHECK(w.model.tab(ada).transcript.size() == 2);     // nothing arrived while down
    std::printf("down for 8 s: %zu datagrams dropped, longest without hearing the server %.1f s\n", relay.dropped,
        double(longestQuiet) / 1000);
    CHECK(relay.dropped > 0);
    CHECK(longestQuiet >= sync::kQuietMs);
    CHECK(!sync::linkStatus(sync::LinkPhase::Connected, "studio", longestQuiet, "").empty());

    // Back up: everything sent arrives once, in order, and the window converges.
    relay.down = false;
    const auto back = Clock::now();
    CHECK(settle([&] { return w.model.tab(ada).transcript.size() == 8 && w.session->allAcknowledged(); }));
    std::printf("converged %.2f s after the network came back\n", seconds(back));
    const auto& t = w.model.tab(ada).transcript;
    const std::vector<std::string> want = {"before", "during 0", "during 1", "during 2"};
    CHECK(t.size() == 2 * want.size());
    for (std::size_t i = 0; i < t.size() && i < 2 * want.size(); ++i)
        CHECK(i % 2 == 0 ? t[i].text == want[i / 2] : t[i].kind == EntryKind::Notice && noDriver(t[i].text));
    const sync::SessionView* v = w.session->state().find(sid("ada"));
    CHECK(sessionsAsMinted(w.session->state()));
    CHECK(v != nullptr && v->total() == 8);

    // A second, shorter cut, with a prompt queued in it.
    relay.down = true;
    const auto cut2 = Clock::now();
    w.session->add(act(sync::ActionKind::Queue, sid("ada"), "queued while down"));
    while (seconds(cut2) < 2)
        step(windows, &relay, 50);
    relay.down = false;
    CHECK(settle([&] { return w.model.tab(ada).queue.size() == 1 && w.session->allAcknowledged(); }));
    CHECK(w.model.tab(ada).transcript.size() == 8);
    w.session->shutdown();
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end && !w.session->shutdownDone())
        step(windows, &relay, 50);
    std::printf("outage: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}

// ---- soak ----

// The entries a window shows, by the server's transcript index, with the entries it never received (the notice of
// how many came before) left empty.
std::vector<const Entry*> byIndex(const Tab& tab) {
    std::vector<const Entry*> out;
    for (const Entry& e : tab.transcript) {
        if (e.kind == EntryKind::Notice && !noDriver(e.text)) {
            const std::size_t n = std::strtoul(e.text.c_str(), nullptr, 10);
            out.insert(out.end(), n, nullptr);
        } else {
            out.push_back(&e);
        }
    }
    return out;
}

int soak(const std::string& binary, const std::string& dir, int clients, int prompts) {
    Server server(binary, dir);
    std::vector<std::unique_ptr<Window>> owned;
    std::vector<Window*> windows;
    const auto start = Clock::now();
    for (int c = 0; c < clients; ++c) {
        owned.push_back(std::make_unique<Window>());
        link::Connect connect;
        if (!openWindow(server, "localhost", "127.0.0.1", nullptr, owned.back().get(), &connect))
            return 1;
        windows.push_back(owned.back().get());
    }
    std::printf("%d windows connected in %.2f s\n", clients, seconds(start));
    for (Window* w : windows) {
        for (const sync::Action& a : ensures()) {
            w->session->add(a);
        }
    }
    test::Rng rng{42};
    std::map<std::string, int> sends;
    std::map<std::string, int> queues;
    const auto sending = Clock::now();
    // The longest any window goes without a new state from the server, from the first prompt to convergence: what a
    // person at that window would see as the server not answering.
    std::vector<std::uint64_t> heardNum;
    for (Window* w : windows)
        heardNum.push_back(w->session->stateNum());
    std::vector<Clock::time_point> heardAt(windows.size(), sending);
    double longestGap = 0;
    auto watch = [&] {
        for (std::size_t i = 0; i < windows.size(); ++i) {
            const std::uint64_t n = windows[i]->session->stateNum();
            if (n != heardNum[i]) {
                longestGap = std::max(longestGap, std::chrono::duration<double>(Clock::now() - heardAt[i]).count());
                heardNum[i] = n;
                heardAt[i] = Clock::now();
            }
        }
    };
    std::vector<int> next(static_cast<std::size_t>(clients), 0);
    for (int p = 0; p < prompts; ++p) {
        const std::size_t c = rng.below(static_cast<std::size_t>(clients));
        const std::string tab = kFriends[rng.below(kFriends.size())].name;
        const std::string text = "c" + std::to_string(c) + "-" + std::to_string(next[c]++);
        // A few queued prompts, within the queue's limit of eight per tab.
        if (rng.chance(20) && queues[tab] < 8) {
            windows[c]->session->add(act(sync::ActionKind::Queue, sid(tab), text));
            ++queues[tab];
        } else {
            windows[c]->session->add(act(sync::ActionKind::Send, sid(tab), text));
            ++sends[tab];
        }
        if (rng.chance(4)) {
            step(windows, nullptr, 0);
            watch();
        }
    }
    auto converged = [&] {
        for (Window* w : windows) {
            if (!w->session->allAcknowledged())
                return false;
            for (const FriendInfo& f : kFriends) {
                const sync::SessionView* v = w->session->state().find(sid(f.name));
                const std::size_t want = 2 * static_cast<std::size_t>(sends[f.name]);
                if ((v == nullptr ? 0 : v->total()) != want ||
                        (v == nullptr ? 0 : v->queue.size()) != static_cast<std::size_t>(queues[f.name]))
                    return false;
            }
        }
        return true;
    };
    // Every window hears every state the server makes: the sum of their state numbers moves with each applied prompt.
    CHECK(whileProgressing(converged, [&] {
        std::uint64_t sum = 0;
        for (Window* w : windows)
            sum += w->session->stateNum();
        return sum;
    }, [&] { step(windows, nullptr, 20); watch(); }));
    std::printf("%d prompts from %d windows applied and seen by every window in %.2f s; the longest any window went "
        "without a new state: %.2f s\n", prompts, clients, seconds(sending), longestGap);
    // Every window holds the same state (the server's), and its view agrees with every other's.
    for (Window* w : windows)
        CHECK(w->session->state() == windows[0]->session->state());
    CHECK(sessionsAsMinted(windows[0]->session->state()));
    for (const FriendInfo& f : kFriends) {
        std::vector<std::vector<const Entry*>> views;
        for (Window* w : windows)
            views.push_back(byIndex(w->model.tab(tabOf(w->model, f.name))));
        const std::size_t total = 2 * static_cast<std::size_t>(sends[f.name]);
        std::size_t seenPrompts = 0;
        for (std::size_t v = 0; v < views.size(); ++v) {
            CHECK(views[v].size() == total);
            std::map<std::string, int> count;
            std::vector<int> lastSeq(static_cast<std::size_t>(clients), -1);
            for (std::size_t i = 0; i < views[v].size(); ++i) {
                const Entry* e = views[v][i];
                if (e == nullptr)
                    continue;
                // the same entry at this index in every window that shows it
                for (const auto& other : views) {
                    if (i < other.size() && other[i] != nullptr)
                        CHECK(other[i]->kind == e->kind && other[i]->text == e->text);
                }
                if (i % 2 == 1) {
                    CHECK(e->kind == EntryKind::Notice && noDriver(e->text));
                    continue;
                }
                CHECK(e->kind == EntryKind::Person);
                CHECK(++count[e->text] == 1);   // once
                const std::size_t dash = e->text.find('-');
                const std::size_t c = std::strtoul(e->text.c_str() + 1, nullptr, 10);
                const int seq = std::atoi(e->text.c_str() + dash + 1);
                if (c < lastSeq.size()) {
                    CHECK(seq > lastSeq[c]);    // each window's prompts in its order
                    lastSeq[c] = seq;
                }
                ++seenPrompts;
            }
            CHECK(windows[v]->model.tab(tabOf(windows[v]->model, f.name)).queue.size() ==
                static_cast<std::size_t>(queues[f.name]));
        }
        std::printf("tab %s: %d sent, %d queued; the windows show %zu of the sent prompts between them\n",
            f.name.c_str(), sends[f.name], queues[f.name], seenPrompts);
    }
    for (Window* w : windows)
        w->session->shutdown();
    const auto end = Clock::now() + std::chrono::seconds(3);
    auto done = [&] {
        for (Window* w : windows) {
            if (!w->session->shutdownDone())
                return false;
        }
        return true;
    };
    while (Clock::now() < end && !done())
        step(windows, nullptr, 20);
    std::printf("soak: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}

// ---- restart ----

// `nexus-server serve --dir <dir>` as this test's child, its output to the server's log, so the test can kill it; its
// HOME is `dir`/home, as the bootstrap's is, never the machine's.
pid_t startServe(const std::string& binary, const std::string& dir) {
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    const std::string log = dir + "/server.log";
    posix_spawn_file_actions_addopen(&files, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&files, 1, 2);
    std::vector<std::string> args = {binary, "serve", "--dir", dir};
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env = {"HOME=" + dir + "/home"};
    for (char** e = environ; *e != nullptr; ++e) {
        if (std::strncmp(*e, "HOME=", 5) != 0) {
            env.emplace_back(*e);
        }
    }
    std::vector<char*> envp;
    for (std::string& e : env) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);
    pid_t pid = -1;
    if (posix_spawn(&pid, binary.c_str(), &files, nullptr, argv.data(), envp.data()) != 0)
        pid = -1;
    posix_spawn_file_actions_destroy(&files);
    return pid;
}

// A server answers on the socket: a `new` from now on reaches it rather than starting another.
bool answering(const std::string& dir) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = dir + "/server.sock";
    std::memcpy(addr.sun_path, path.c_str(), std::min(path.size() + 1, sizeof addr.sun_path));
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    const bool ok = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
    close(fd);
    return ok;
}

// The server is killed with SIGKILL while a window is connected, and started again; a new session of the same window
// (its client id) sends every one of its actions again, under the same numbers, with one new one after them. Every
// session, queue and applied counter came through the kill: the state is all there, and only the new action is applied.
int restart(const std::string& binary, const std::string& dir) {
    Server server(binary, dir);
    mkdir(dir.c_str(), 0700);
    pid_t pid = startServe(binary, dir);
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, kStallSeconds));
    const std::vector<std::pair<std::string, std::string>> before = {{"ada", "one"}, {"bo", "later"}, {"ada", "two"},
                                                                    {"ada", "three"}};
    auto session = [&](std::uint64_t next, Window* w) {
        link::Bootstrap b = server.bootstrap("localhost");
        b.command.insert(b.command.end(), {"--client", "window-1", "--next", std::to_string(next)});
        link::Connect connect;
        std::string why;
        if (!link::bootstrap(b, runCommand, &connect, &why)) {
            std::fprintf(stderr, "link_e2e: %s\n", why.c_str());
            return false;
        }
        w->session = std::make_unique<link::ClientSession>(connect.key, "127.0.0.1", connect.port, next);
        return true;
    };
    auto add = [](Window& w, const std::pair<std::string, std::string>& a) {
        w.session->add(act(a.first == "bo" ? sync::ActionKind::Queue : sync::ActionKind::Send, sid(a.first), a.second));
    };
    auto settle = [](Window& w, const std::function<bool()>& done) {
        std::vector<Window*> ws{&w};
        return whileProgressing(done, [&] { return w.session->stateNum() * 2 + (w.session->allAcknowledged() ? 1 : 0); },
            [&] { step(ws, nullptr, 20); });
    };
    // For as long as `seconds`, whatever happens: the timing the run itself is about.
    auto run = [](Window& w, int seconds) {
        std::vector<Window*> ws{&w};
        const auto end = Clock::now() + std::chrono::seconds(seconds);
        while (Clock::now() < end)
            step(ws, nullptr, 20);
    };
    // ada's texts, with the notice that ada has no driver as "no driver".
    auto adaTexts = [](const sync::Sessions& sessions) {
        std::vector<std::string> out;
        if (const sync::SessionView* v = sessions.find(sid("ada"))) {
            for (const sync::EntryRef& e : v->entries)
                out.push_back(e->kind == EntryKind::Notice && noDriver(e->text) ? "no driver" : e->text);
        }
        return out;
    };
    {
        Window w;
        if (!session(1, &w))
            return 1;
        for (const sync::Action& a : ensures()) {
            w.session->add(a);
        }
        for (std::size_t i = 0; i < 3; ++i)
            add(w, before[i]);
        CHECK(settle(w, [&] { return w.session->allAcknowledged() && adaTexts(w.session->state()).size() == 4; }));
        // The fourth is sent and the server killed at once: applied and durable, or not applied at all.
        add(w, before[3]);
        run(w, 1);
    }
    CHECK(kill(pid, SIGKILL) == 0);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    std::printf("server killed with SIGKILL\n");

    pid = startServe(binary, dir);
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, kStallSeconds));
    Window w;
    if (!session(1, &w))
        return 1;
    // The fresh sync: what was acknowledged before the kill is there before the window sends anything.
    CHECK(settle(w, [&] { return w.session->stateNum() > 0 && adaTexts(w.session->state()).size() >= 4; }));
    const std::vector<std::string> first = adaTexts(w.session->state());
    std::printf("after the restart the server holds %zu entries in ada\n", first.size());
    CHECK(first.size() == 4 || first.size() == 6);
    CHECK(sessionsAsMinted(w.session->state()));
    const sync::SessionView* bo = w.session->state().find(sid("bo"));
    CHECK(bo != nullptr && bo->queue == std::vector<std::string>{"later"});
    // Every action again, under its own number (the ensures first: they make nothing now), and an eighth.
    for (const sync::Action& a : ensures()) {
        w.session->add(a);
    }
    for (const auto& a : before)
        add(w, a);
    add(w, {"ada", "four"});
    const std::vector<std::string> want = {"one", "no driver", "two", "no driver", "three", "no driver",
                                           "four", "no driver"};
    CHECK(settle(w, [&] { return w.session->allAcknowledged() && adaTexts(w.session->state()) == want; }));
    CHECK(adaTexts(w.session->state()) == want);
    bo = w.session->state().find(sid("bo"));
    CHECK(sessionsAsMinted(w.session->state()));
    CHECK(bo != nullptr && bo->queue == std::vector<std::string>{"later"});
    CHECK(w.model.tab(tabOf(w.model, "ada")).transcript.size() == want.size());
    // A session that starts past what the server applied would skip actions: refused.
    {
        link::Bootstrap b = server.bootstrap("localhost");
        b.command.insert(b.command.end(), {"--client", "window-1", "--next", "12"});
        link::Connect connect;
        std::string why;
        CHECK(!link::bootstrap(b, runCommand, &connect, &why));
        CHECK(why.find("applied its actions up to 8") != std::string::npos);
    }
    w.session->shutdown();
    std::vector<Window*> ws{&w};
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end && !w.session->shutdownDone())
        step(ws, nullptr, 20);
    std::printf("restart: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;   // the Server stops the restarted server
}

} // namespace

// ---- flush: nothing of a batch leaves the server before its flush ----

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

std::size_t count(const std::string& text, const std::string& part) {
    std::size_t n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
        ++n;
    }
    return n;
}

// The real server, with the store's flush gated (server/flush_gate.h). ada's driver (the fake) has answered "zero" and
// is idle, so a prompt to her is written to it in the same pass that applies the window's action. The gate holds the
// batch with "one" after its write and before its flush: for as long as it is held the driver has not been sent
// "one", the window has no state that shows it, and the action is not acknowledged. Then the batch is lost (the log
// cut back to its last flush, the server killed). Started again, the server holds "zero" and not "one"; the window
// sends its actions again under their numbers, "one" is applied, and the driver is sent each prompt once.
int flush(const std::string& binary, const std::string& dir, const std::string& fake) {
    Server server(binary, dir);
    mkdir(dir.c_str(), 0700);
    const std::string sent = dir + "/sent";
    const std::string reply = "send\nrecord-send " + sent + "\nemit {\"kind\":\"text\",\"delta\":\"back\"}\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n";
    CHECK(writeFile(dir + "/ada.script", "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\",\"features\":[\"limit\"]}\n"
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n" + reply + reply));
    CHECK(writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script\nfriend ada fake " + dir +
        "\n"));
    const std::string gate = dir + "/gate";
    ::setenv("NEXUS_TEST_FLUSH_GATE", gate.c_str(), 1);
    pid_t pid = startServe(binary, dir);
    ::unsetenv("NEXUS_TEST_FLUSH_GATE");
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, kStallSeconds));
    auto session = [&](Window* w) {
        link::Bootstrap b = server.bootstrap("localhost");
        b.command.insert(b.command.end(), {"--client", "window-1", "--next", "1"});
        link::Connect connect;
        std::string why;
        if (!link::bootstrap(b, runCommand, &connect, &why)) {
            std::fprintf(stderr, "link_e2e: %s\n", why.c_str());
            return false;
        }
        w->session = std::make_unique<link::ClientSession>(connect.key, "127.0.0.1", connect.port, 1);
        return true;
    };
    auto settle = [](Window& w, const std::function<bool()>& done) {
        std::vector<Window*> ws{&w};
        return whileProgressing(done, [&] { return w.session->stateNum() * 2 + (w.session->allAcknowledged() ? 1 : 0); },
            [&] { step(ws, nullptr, 20); });
    };
    auto adaTexts = [](const sync::Sessions& sessions) {
        std::vector<std::string> out;
        if (const sync::SessionView* v = sessions.find(sid("ada"))) {
            for (const sync::EntryRef& e : v->entries)
                out.push_back(e->text);
        }
        return out;
    };
    auto adaIdle = [](const sync::Sessions& sessions) {
        const sync::SessionView* v = sessions.find(sid("ada"));
        return v != nullptr && v->activity == sync::Activity::Idle;
    };
    auto all = [](Window& w, bool withOne) {
        for (const sync::Action& a : ensures()) {
            w.session->add(a);
        }
        w.session->add(act(sync::ActionKind::Send, sid("ada"), "zero"));
        if (withOne) {
            w.session->add(act(sync::ActionKind::Send, sid("ada"), "one"));
        }
    };
    const std::vector<std::string> answered = {"zero", "back"};
    {
        Window w;
        if (!session(&w))
            return 1;
        all(w, false);
        CHECK(settle(w, [&] { return w.session->allAcknowledged() && adaTexts(w.session->state()) == answered &&
            adaIdle(w.session->state()); }));
        CHECK(count(readFile(sent), "\nzero\n") == 1);
        // The next batch's flush is held.
        CHECK(writeFile(gate, "hold\n"));
        w.session->add(act(sync::ActionKind::Send, sid("ada"), "one"));
        std::vector<Window*> ws{&w};
        CHECK(waitFor([&] { step(ws, nullptr, 20); return ::access((gate + ".reached").c_str(), F_OK) == 0; },
            kStallSeconds));
        // Held, a while: the driver would have written what it was sent by now, and the window received what was sent.
        const auto end = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < end) {
            step(ws, nullptr, 20);
        }
        CHECK(count(readFile(sent), "\none\n") == 0);
        CHECK(adaTexts(w.session->state()) == answered);
        CHECK(!w.session->allAcknowledged());
        std::printf("held between the batch's write and its flush: nothing of it left the server\n");
        // The machine stops before the flush.
        CHECK(writeFile(gate, "lose\n"));
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(count(readFile(sent), "\none\n") == 0);
    ::unlink(gate.c_str());

    pid = startServe(binary, dir);
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, kStallSeconds));
    Window w;
    if (!session(&w))
        return 1;
    // What the server holds after the crash: the durable batches, not the lost one.
    CHECK(settle(w, [&] { return w.session->stateNum() > 0 && adaTexts(w.session->state()).size() >= 2; }));
    CHECK(adaTexts(w.session->state()) == answered);
    all(w, true);
    const std::vector<std::string> want = {"zero", "back", "one", "back"};
    CHECK(settle(w, [&] { return w.session->allAcknowledged() && adaTexts(w.session->state()) == want; }));
    CHECK(adaTexts(w.session->state()) == want);
    CHECK(count(readFile(sent), "\nzero\n") == 1);
    CHECK(count(readFile(sent), "\none\n") == 1);
    w.session->shutdown();
    std::vector<Window*> ws{&w};
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end && !w.session->shutdownDone())
        step(ws, nullptr, 20);
    std::printf("flush: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;   // the Server stops the restarted server
}

// ---- secret: a value pasted in a window reaches its account's file and nothing else ----

// A fake value of `size` bytes, made up here from `seed`: letters and digits in no pattern any other text holds.
std::string fakeValue(std::uint64_t seed, std::size_t size) {
    const char* const letters = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::string out;
    std::uint64_t x = seed;
    while (out.size() < size) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        out += letters[(x >> 33) % 56];
    }
    return out;
}

// Every regular file under `dir` but `except`, with `scan` run over each: what gives the value away, and where.
std::vector<std::string> leaksUnder(const std::string& dir, const std::string& except, const test::LeakScan& scan,
                                    int* read) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        // A link is not a file the run wrote (the signin run's bin/claude-helper is a link to the fake program, whose
        // own made-up values are in it).
        if (entry.is_symlink() || !entry.is_regular_file() || entry.path() == std::filesystem::path(except)) {
            continue;
        }
        ++*read;
        const std::string what = scan.find(readFile(entry.path().string()));
        if (!what.empty()) {
            found.push_back(entry.path().string() + ": " + what);
        }
    }
    return found;
}

// The real nexus-server (started by the test, its standard output and error to server.log) with a HOME of the test's
// own under <dir>, never the machine's: its account a1 (claude) holds an old fake login, so ada's driver (the fake,
// standing in for the claude harness) starts, and on the prompt asks for a1's oauth-token. The window, over the real
// link (ssh, mosh's transport, ServerSession::take on the server), sees the request in its state and answers it with a
// new fake value of 7919 bytes. The driver hears "stored" and nothing more; the file holds the value, mode 0600 in a
// 0700 directory. Then, with the server stopped, every other byte the run left is searched for the value (any run of 8
// of its bytes, its hex, its base64, its length beside the account's name; tests/leak_scan.h): server.log (the
// server's standard error and the driver's, which echoes each line it reads), the driver's environment, what it read,
// the store, the config, the drivers file, and the window's own state.
int secret(const std::string& binary, const std::string& dir, const std::string& fake) {
    Server server(binary, dir);
    CHECK(::mkdir(dir.c_str(), 0700) == 0);
    const std::string home = dir + "/home";
    const std::string accounts = home + "/.nexus/server/accounts";
    const std::string file = accounts + "/a1/oauth-token";
    std::error_code ec;
    std::filesystem::create_directories(accounts + "/a1", ec);
    for (const std::string& d : {home, home + "/.nexus", home + "/.nexus/server", accounts, accounts + "/a1"}) {
        CHECK(::chmod(d.c_str(), 0700) == 0);
    }
    const std::string oldValue = fakeValue(2027, 3001);
    const std::string value = fakeValue(7919, 7919);
    CHECK(writeFile(file, oldValue));
    CHECK(::chmod(file.c_str(), 0600) == 0);
    const std::string heard = dir + "/heard";
    CHECK(writeFile(dir + "/ada.script", "option echo-input\nrecord-env " + dir + "/env\n"
        "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\",\"features\":[\"limit\"]}\n"
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n"
        "send\nstderr the fake driver asks for a1's login\n"
        "raw {\"kind\":\"ask-secret\",\"ask\":1,\"account\":\"a1\",\"what\":\"oauth-token\",\"why\":\"the login expired\"}\n"
        "await-secret " + heard + "\n"
        "emit {\"kind\":\"text\",\"delta\":\"signed in again\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n"));
    CHECK(writeFile(dir + "/drivers", "driver claude " + fake + " --script " + dir + "/ada.script --account a1\n"
        "friend ada claude " + dir + "\n"));
    pid_t pid = startServe(binary, dir);
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, kStallSeconds));

    Window w;
    {
        const link::Bootstrap b = server.bootstrap("localhost");
        link::Connect connect;
        std::string why;
        if (!link::bootstrap(b, runCommand, &connect, &why)) {
            std::fprintf(stderr, "link_e2e: %s\n", why.c_str());
            return 1;
        }
        CHECK(connect.format >= sync::kSecretsFormat);
        w.session = std::make_unique<link::ClientSession>(connect.key, "127.0.0.1", connect.port, 1, connect.format);
    }
    std::vector<Window*> ws{&w};
    auto settle = [&](const std::function<bool()>& done) {
        return whileProgressing(done, [&] { return w.session->stateNum() * 2 + (w.session->allAcknowledged() ? 1 : 0); },
            [&] { step(ws, nullptr, 20); });
    };
    std::vector<std::string> states;   // every state the window held, as text
    for (const sync::Action& a : ensures()) {
        w.session->add(a);
    }
    w.session->add(act(sync::ActionKind::Send, sid("ada"), "please sign in again"));
    CHECK(settle([&] { return !w.session->state().requests().empty(); }));
    states.push_back(w.session->state().init_diff());
    std::uint64_t id = 0;
    if (w.session->state().requests().size() == 1) {
        const sync::SecretRequest r = w.session->state().requests()[0];
        CHECK(r.account == "a1" && r.what == "oauth-token" && r.askerName == "ada" && r.why == "the login expired");
        id = r.id;
    }
    CHECK(id != 0);
    // The person pastes the value and presses Enter: the window's answer goes up the link.
    sync::Action answer;
    answer.kind = sync::ActionKind::SecretAnswer;
    answer.request = id;
    answer.secret = sync::SecretValue(value);
    w.session->add(std::move(answer));
    CHECK(settle([&] {
        const sync::SessionView* v = w.session->state().find(sid("ada"));
        return w.session->allAcknowledged() && w.session->state().requests().empty() && v != nullptr &&
            v->activity == sync::Activity::Idle && v->total() >= 3;
    }));
    states.push_back(w.session->state().init_diff());
    CHECK(readFile(heard).find("\"stored\"") != std::string::npos);
    CHECK(readFile(file) == value);
    struct stat st{};
    CHECK(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(::stat((accounts + "/a1").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(accounts + "/a1", ec)) {
        names.push_back(entry.path().filename().string());
    }
    CHECK(names == std::vector<std::string>{"oauth-token"});
    w.session->shutdown();
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end && !w.session->shutdownDone()) {
        step(ws, nullptr, 20);
    }
    // The server stops (and its driver with it), so every file it and the driver wrote is complete.
    const CommandResult stopped = runCommand(Command{binary, {"stop", "--dir", dir}, binary + " stop --dir " + dir}, 10);
    CHECK(stopped.end == CommandResult::End::Exited && stopped.code == 0);
    int status = 0;
    waitpid(pid, &status, 0);
    const std::string log = readFile(dir + "/server.log");
    CHECK(log.find("the fake driver asks for a1's login") != std::string::npos);   // the driver's stderr is in it
    CHECK(log.find("fake driver read: ") != std::string::npos);                     // and what it read, echoed
    CHECK(readFile(dir + "/env").find("HOME=" + home) != std::string::npos);
    int read = 0;
    for (const std::string& v : {value, oldValue}) {
        const test::LeakScan scan(v, "a1");
        for (const std::string& where : leaksUnder(dir, file, scan, &read)) {
            std::fprintf(stderr, "link_e2e: a value reached %s\n", where.c_str());
            CHECK(false);
        }
        for (const std::string& s : states) {
            CHECK(!scan.in(s));
        }
        CHECK(!scan.in(stopped.out) && !scan.in(stopped.err));
    }
    CHECK(read > 10);
    std::printf("secret: %d files searched, %zu bytes of server.log; %s\n", read / 2, log.size(),
        failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;   // the Server prints server.log and removes the directory
}

// ---- friends: the strip built from the server's list of friends ----

// One headless window as the app opens it on a fresh install: no friend compiled in or read from any outside tool, no
// sprint board, every friend tab built from the list the server sends by the window's own code (sync/friend_strip.h
// takeFriends, which FrameHost::takeFriends calls): an ensure sent for each friend whose tab is added, and the mirror
// told of each tab removed.
struct ServerWindow {
    std::unique_ptr<link::ClientSession> session;
    Model model{{}, false};
    sync::Mirror mirror;
    sync::FriendStrip strip;
    std::uint64_t mirroredNum = ~std::uint64_t(0);
    void after() {
        if (session->stateNum() == mirroredNum) {
            return;
        }
        mirroredNum = session->stateNum();
        const sync::Sessions& state = session->state();
        sync::takeFriends(strip, model, mirror, state, [this](sync::Action a) { session->add(a); });
        mirror.apply(model, state);
    }
};

// The friend tabs' names, in the strip's order.
std::vector<std::string> friendTabs(const Model& m) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        if (m.tab(i).kind == TabKind::Friend) {
            out.push_back(m.tab(i).name);
        }
    }
    return out;
}

// A fresh server under a HOME of its own in <dir>, with one friend, ada, on the fake driver (its drivers file seeds
// friends.json at the first start). A window that knows no friend connects: its strip shows ada's tab alone, a prompt
// typed there starts ada's session on the server and its driver, and the reply comes back. Then friends.json is
// edited as a person would and `nexus-server config reload` run: bo's tab appears in the open window, and once bo is
// taken out again and the config reloaded, bo's tab goes, ada's staying as it was.
int friendsFromServer(const std::string& binary, const std::string& dir, const std::string& fake) {
    Server server(binary, dir);
    CHECK(::mkdir(dir.c_str(), 0700) == 0);
    const std::string home = dir + "/home";
    std::error_code ec;
    std::filesystem::create_directories(home, ec);
    const std::string reply = "send\nemit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"hello back\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n";
    CHECK(writeFile(dir + "/ada.script", "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\n"
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n" + reply));
    CHECK(writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script\nfriend ada fake " + dir +
        "\n"));
    pid_t pid = startServe(binary, dir);
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, kStallSeconds));

    ServerWindow w;
    {
        const link::Bootstrap b = server.bootstrap("localhost");
        link::Connect connect;
        std::string why;
        if (!link::bootstrap(b, runCommand, &connect, &why)) {
            std::fprintf(stderr, "link_e2e: %s\n", why.c_str());
            return 1;
        }
        w.session = std::make_unique<link::ClientSession>(connect.key, "127.0.0.1", connect.port, 1, connect.format);
    }
    // One round, as step() does for the other runs' windows: tick, poll for up to 20 ms, read, then take the state.
    auto stepOnce = [&] {
        link::freezeClock();
        std::string why;
        w.session->tick(&why);
        std::vector<pollfd> fds;
        for (int fd : w.session->fds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::min(20, std::max(0, w.session->waitMs())));
        link::freezeClock();
        for (const pollfd& p : fds) {
            if (p.revents & POLLIN) {
                w.session->receive(&why);
            }
        }
        w.after();
    };
    // Progress is a change in what the server holds or in what it has acknowledged; a new state number alone is not
    // (the server's state numbers move on without anything in it changing), so a wait for a tab that never comes
    // fails kStallSeconds after the last change rather than at ctest's backstop.
    auto settle = [&](const std::function<bool()>& done) {
        return whileProgressing(done, [&] {
                return std::hash<std::string>{}(w.session->state().init_diff()) * 2 +
                    (w.session->allAcknowledged() ? 1 : 0);
            }, stepOnce);
    };

    // The fresh window shows ada's tab alone, from the server's list, and its ensure starts ada's session.
    CHECK(friendTabs(w.model).empty());
    CHECK(settle([&] { return w.session->state().friendsKnown() && w.session->state().first("ada") != nullptr; }));
    CHECK((friendTabs(w.model) == std::vector<std::string>{"ada"}));
    CHECK(w.model.tab_count() == 1);
    const std::size_t ada = tabOf(w.model, "ada");
    const std::size_t adaId = w.model.tab(ada).id;
    const sync::SessionView* v = w.session->state().first("ada");
    CHECK(v != nullptr);
    const sync::SessionId adaSession = v != nullptr ? v->id : 0;

    // A prompt typed in it reaches ada's driver, and its reply comes back to the tab.
    w.session->add(act(sync::ActionKind::Send, adaSession, "hello"));
    CHECK(settle([&] {
        const sync::SessionView* s = w.session->state().find(adaSession);
        return w.session->allAcknowledged() && s != nullptr && s->activity == sync::Activity::Idle &&
            w.model.tab(tabOf(w.model, "ada")).transcript.size() == 2;
    }));
    const std::vector<Entry>& t = w.model.tab(tabOf(w.model, "ada")).transcript;
    CHECK(t.size() == 2 && t[0].text == "hello" && t[1].text == "hello back");

    // bo added to friends.json and the config reloaded: bo's tab appears in the open window, after ada's.
    const std::string friendsJson = dir + "/nexus-config/friends.json";
    const std::string seeded = readFile(friendsJson);
    const std::string list = "\"friends\": [";
    const std::size_t at = seeded.find(list);
    CHECK(at != std::string::npos);
    const std::string withBo = at == std::string::npos ? seeded : seeded.substr(0, at + list.size()) +
        "\n    {\"name\": \"bo\", \"accounts\": [], \"default\": null, \"instances\": []}," +
        seeded.substr(at + list.size());
    const Command reload{"/usr/bin/env", {"HOME=" + home, binary, "config", "reload", "--dir", dir},
        "nexus-server config reload"};
    CHECK(writeFile(friendsJson, withBo));
    CommandResult r = runCommand(reload, 20);
    if (r.end != CommandResult::End::Exited || r.code != 0) {
        std::fprintf(stderr, "link_e2e: reload with bo: %s%s\n", r.out.c_str(), r.err.c_str());
        CHECK(false);
    }
    CHECK(settle([&] { return friendTabs(w.model).size() == 2; }));
    CHECK((friendTabs(w.model) == std::vector<std::string>{"ada", "bo"}));

    // bo taken out and the config reloaded: bo's tab goes, what the person typed to bo kept by name; ada's tab is the
    // same tab, its transcript kept.
    CHECK(w.model.set_draft(tabOf(w.model, "bo"), "half a prompt for bo"));
    CHECK(writeFile(friendsJson, seeded));
    r = runCommand(reload, 20);
    if (r.end != CommandResult::End::Exited || r.code != 0) {
        std::fprintf(stderr, "link_e2e: reload without bo: %s%s\n", r.out.c_str(), r.err.c_str());
        CHECK(false);
    }
    CHECK(settle([&] { return friendTabs(w.model).size() == 1; }));
    CHECK((friendTabs(w.model) == std::vector<std::string>{"ada"}));
    CHECK(w.model.tab(tabOf(w.model, "ada")).id == adaId);
    CHECK(w.model.tab(tabOf(w.model, "ada")).transcript.size() == 2);
    CHECK(w.strip.kept().count("bo") == 1 && w.strip.kept().at("bo") == "half a prompt for bo");

    // bo back in friends.json: a new tab for bo, with the draft the person left in the old one.
    CHECK(writeFile(friendsJson, withBo));
    r = runCommand(reload, 20);
    if (r.end != CommandResult::End::Exited || r.code != 0) {
        std::fprintf(stderr, "link_e2e: reload with bo again: %s%s\n", r.out.c_str(), r.err.c_str());
        CHECK(false);
    }
    CHECK(settle([&] { return friendTabs(w.model).size() == 2; }));
    CHECK((friendTabs(w.model) == std::vector<std::string>{"ada", "bo"}));
    CHECK(w.model.tab(tabOf(w.model, "bo")).draft == "half a prompt for bo" && w.strip.kept().empty());

    w.session->shutdown();
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end && !w.session->shutdownDone()) {
        stepOnce();
    }
    const CommandResult stopped = runCommand(Command{binary, {"stop", "--dir", dir}, binary + " stop --dir " + dir}, 10);
    CHECK(stopped.end == CommandResult::End::Exited && stopped.code == 0);
    int status = 0;
    waitpid(pid, &status, 0);
    std::printf("friends: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;   // the Server prints server.log and removes the directory
}

// ---- signin: in-app sign-in through the real server and the real link, with the FAKE claude ----

// The fake's values (tests/server/fake_claude.h), repeated here so that this file needs nothing of the server's tests.
const char* const kFakeState = "fakeStateQm8vTrXw4kGdHbN2pL7cJ9sZ1aE6";
const char* const kFakeCode = "fakeCodePw3kGdHbX5rT8yU2iO4mN7qW1eR9";
const char* const kFakeToken = "sk-ant-oat01-FAKEtokenZq8wX3cV6bN9mA2sD5fG7hJ1kL4pO0iUyTrEwQaSdFgHjKlZxCvBnMwPeRtYuIoPaSdFgHjKlZxCvBnMqWe-AA";

const sync::SignInOffer* offerOf(const sync::Sessions& s, std::uint64_t id) {
    for (const sync::SignInOffer& o : s.offers()) {
        if (o.id == id) {
            return &o;
        }
    }
    return nullptr;
}

sync::Action signInAction(sync::ActionKind kind, std::uint64_t id, const std::string& token,
                          const std::string& query = std::string()) {
    sync::Action a;
    a.kind = kind;
    a.request = id;
    a.text = token;
    if (!query.empty()) {
        a.secret = sync::SecretValue(query);
    }
    return a;
}

// The real nexus-server (started by the test under a HOME of its own in <dir>; its config names the FAKE claude,
// tests/server/fake_claude.cpp, as `claude`) with account a1 and no login, and two windows over the real link (ssh,
// mosh's transport): the flow is offered to both; the first claims it and its state alone carries the URL, the other
// sees it held; the first window's link ends and the flow is let go of; the second takes it over, and its state now
// carries the URL; it sends the callback up as a window on another machine would, the server replays it to the fake's
// listener, and the fake's token lands in a1's file. An AI's `config sign-in a1`, asked while the flow waits, answers
// `signed in` once it does; one for an account not set up is refused. Then, the server stopped, every file the run
// left (server.log first) and every state the second window held before it held the flow is searched for the URL's
// state, the code and the token.
//
// `leaky`: the server is nexus-server-leaky (CMakeLists.txt), which writes the forwarded query to its log; the run
// passes only when the search finds it there (the search's reversed witness).
int signin(const std::string& binary, const std::string& dir, const std::string& fakeClaude, bool leaky) {
    Server server(binary, dir);
    CHECK(::mkdir(dir.c_str(), 0700) == 0);
    const std::string abs = std::filesystem::absolute(dir).string();
    const std::string home = dir + "/home";
    const std::string accounts = home + "/.nexus/server/accounts";
    const std::string file = accounts + "/a1/oauth-token";
    std::error_code ec;
    std::filesystem::create_directories(accounts + "/a1", ec);
    for (const std::string& d : {home, home + "/.nexus", home + "/.nexus/server", accounts, accounts + "/a1"}) {
        CHECK(::chmod(d.c_str(), 0700) == 0);
    }
    std::filesystem::create_directories(dir + "/bin", ec);
    std::filesystem::create_symlink(fakeClaude, dir + "/bin/claude-helper", ec);
    CHECK(!ec);
    CHECK(writeFile(dir + "/config", "claude " + abs + "/bin/claude-helper\n"));
    // a1 from the drivers file's --account (the first start takes the accounts from it); no session is made, so no
    // driver starts.
    CHECK(writeFile(dir + "/drivers", "driver claude /usr/bin/false --account a1\nfriend ada claude " + abs + "\n"));
    pid_t pid = startServe(binary, dir);
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, kStallSeconds));

    Window w1;
    Window w2;
    for (Window* w : {&w1, &w2}) {
        link::Connect connect;
        if (!openWindow(server, "localhost", "127.0.0.1", nullptr, w, &connect)) {
            return 1;
        }
        CHECK(connect.format >= sync::kSignInFormat);
    }
    std::vector<Window*> ws{&w1, &w2};
    std::vector<std::string> otherStates;      // every state w2 held before it held the flow
    auto settle = [&](const std::function<bool()>& done) {
        return whileProgressing(done, [&] {
            std::uint64_t p = 0;
            for (Window* w : ws) {
                p = p * 1000003 + w->session->stateNum() * 2 + (w->session->allAcknowledged() ? 1 : 0);
            }
            return p;
        }, [&] { step(ws, nullptr, 20); });
    };
    const std::string tok1 = "tokE2eWindowOne1";
    const std::string tok2 = "tokE2eWindowTwo2";

    // Offered to both: flow 1, a1's, held by no window.
    CHECK(settle([&] {
        const sync::SignInOffer* a = offerOf(w1.session->state(), 1);
        const sync::SignInOffer* b = offerOf(w2.session->state(), 1);
        return a != nullptr && b != nullptr && a->opener.empty() && b->opener.empty();
    }));
    // The AI's ask, while the flow waits: it answers once the account is signed in.
    CommandResult asked;
    std::thread ai([&] {
        asked = runCommand(Command{binary, {"config", "sign-in", "a1", "--dir", dir}, "nexus-server config sign-in a1"},
                           120);
    });
    const CommandResult nobody = runCommand(Command{binary, {"config", "sign-in", "nobody", "--dir", dir},
                                                    "nexus-server config sign-in nobody"}, 20);
    CHECK(nobody.end == CommandResult::End::Exited && nobody.code == 1 &&
          nobody.out.find("refused: no account nobody") == 0);

    // Which link gets the URL: the claiming window's alone.
    w1.session->add(signInAction(sync::ActionKind::SignInClaim, 1, tok1));
    CHECK(settle([&] {
        const sync::SignInOffer* a = offerOf(w1.session->state(), 1);
        const sync::SignInOffer* b = offerOf(w2.session->state(), 1);
        otherStates.push_back(w2.session->state().init_diff());
        return a != nullptr && !a->url.empty() && b != nullptr && b->opener == sync::kSignInHeldBy;
    }));
    sync::SignInTarget target;
    if (const sync::SignInOffer* a = offerOf(w1.session->state(), 1)) {
        CHECK(a->opener == tok1 && sync::parseSignInUrl("claude", a->url.bytes(), &target));
        CHECK(target.state == kFakeState);
    }
    if (const sync::SignInOffer* b = offerOf(w2.session->state(), 1)) {
        CHECK(b->url.empty());
    }

    // The first window's link ends: the flow is let go of, for another window to take over.
    w1.session->shutdown();
    const auto end1 = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end1 && !w1.session->shutdownDone()) {
        step(ws, nullptr, 20);
    }
    ws = {&w2};
    CHECK(settle([&] {
        const sync::SignInOffer* b = offerOf(w2.session->state(), 1);
        otherStates.push_back(w2.session->state().init_diff());
        return b != nullptr && b->opener.empty();
    }));

    // The second window takes it over: its state carries the URL now. It opens it, and the person's browser on its
    // machine is sent to the redirect, which its gate takes; it sends the query up.
    w2.session->add(signInAction(sync::ActionKind::SignInClaim, 1, tok2));
    CHECK(settle([&] {
        const sync::SignInOffer* b = offerOf(w2.session->state(), 1);
        return b != nullptr && b->opener == tok2 && !b->url.empty();
    }));
    w2.session->add(signInAction(sync::ActionKind::SignInOpened, 1, tok2));
    w2.session->add(signInAction(sync::ActionKind::SignInCallback, 1, tok2,
                                 std::string("code=") + kFakeCode + "&state=" + kFakeState));
    CHECK(settle([&] {
        return w2.session->allAcknowledged() && offerOf(w2.session->state(), 1) == nullptr &&
            readFile(file) == kFakeToken;
    }));
    ai.join();
    CHECK(asked.end == CommandResult::End::Exited && asked.code == 0 && asked.out == "signed in\n");
    struct stat st{};
    CHECK(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(!std::filesystem::exists(accounts + "/a1/.signin"));

    w2.session->shutdown();
    const auto end2 = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end2 && !w2.session->shutdownDone()) {
        step(ws, nullptr, 20);
    }
    const CommandResult stopped = runCommand(Command{binary, {"stop", "--dir", dir}, binary + " stop --dir " + dir}, 10);
    CHECK(stopped.end == CommandResult::End::Exited && stopped.code == 0);
    int status = 0;
    waitpid(pid, &status, 0);

    const std::string log = readFile(dir + "/server.log");
    CHECK(log.find("sign-in 1: a1: captured") != std::string::npos);
    int read = 0;
    bool logLeaks = false;
    bool otherLeaks = false;
    for (const char* v : {kFakeState, kFakeCode, kFakeToken}) {
        const test::LeakScan scan(v);
        logLeaks = logLeaks || scan.in(log);
        for (const std::string& where : leaksUnder(dir, file, scan, &read)) {
            if (where.rfind(dir + "/server.log", 0) != 0) {
                std::fprintf(stderr, "link_e2e: a value reached %s\n", where.c_str());
                otherLeaks = true;
            }
        }
        for (const std::string& s : otherStates) {
            otherLeaks = otherLeaks || scan.in(s);
        }
        for (const CommandResult* r : {static_cast<const CommandResult*>(&asked), &nobody, &stopped}) {
            otherLeaks = otherLeaks || scan.in(r->out) || scan.in(r->err);
        }
    }
    CHECK(!otherLeaks);
    CHECK(read > 3);
    if (leaky) {
        // The reversed witness: the leaky build's log line is found, so the search's silence below means something.
        CHECK(logLeaks);
        // Its log line is the leak: blanked, so that the Server's printing of server.log shows none of it.
        std::ofstream(dir + "/server.log", std::ios::trunc).flush();
    } else {
        CHECK(!logLeaks);
    }
    std::printf("signin%s: %d files searched, %zu bytes of server.log, %zu states of the other window; %s\n",
                leaky ? " (leaky witness)" : "", read / 3, log.size(), otherStates.size(),
                failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;   // the Server prints server.log and removes the directory
}

// ---- older_server: nexus-server new of this build asking a server from before formats ----

// A stand-in for a running server from before formats, on <dir>/server.sock: it answers each request as such a server
// does, refusing one that states a format with its "unknown request" line, and answering "new" with a CONNECT line
// that states none. `nexus-server new`, asked by a window that states format 2, must ask again without it and print
// that line as it is, which the window reads as format 1.
int olderServer(const std::string& binary, const std::string& dir) {
    stopIn(binary, dir);
    CHECK(::mkdir(dir.c_str(), 0700) == 0);
    const std::string path = dir + "/server.sock";
    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(listen(listener, 4) == 0);
    std::vector<std::string> requests;
    std::thread older([&] {
        for (int n = 0; n < 2; ++n) {
            pollfd p{listener, POLLIN, 0};
            if (::poll(&p, 1, 10000) <= 0) {
                return;
            }
            const int fd = accept(listener, nullptr, nullptr);
            std::string line;
            char c = 0;
            while (read(fd, &c, 1) == 1 && c != '\n') {
                line += c;
            }
            requests.push_back(line);
            const std::string reply = line == "new" ? "NEXUS CONNECT 60001 AbCdEfGhIjKlMnOpQrSt+/\n" :
                "ERROR unknown request; the requests are `new`, `new <client> <next>` and `stop`\n";
            [[maybe_unused]] ssize_t w = write(fd, reply.data(), reply.size());
            close(fd);
        }
    });
    const Command c{"/usr/bin/env", {"NEXUS_STATE_FORMAT=2", binary, "new", "--dir", dir},
        "env NEXUS_STATE_FORMAT=2 nexus-server new"};
    const CommandResult r = runCommand(c, 20);
    older.join();
    close(listener);
    CHECK(r.end == CommandResult::End::Exited && r.code == 0);
    CHECK(r.out == "NEXUS CONNECT 60001 AbCdEfGhIjKlMnOpQrSt+/\n");
    CHECK((requests == std::vector<std::string>{"new format 2", "new"}));
    link::Connect connect;
    CHECK(link::parseConnect("NEXUS CONNECT 60001 AbCdEfGhIjKlMnOpQrSt+/", &connect) && connect.format == 1);
    Command rm{"/bin/rm", {"-rf", dir}, "rm"};
    runCommand(rm, 5);
    std::printf("older_server: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[2], "stop") == 0) {
        int status = 0;
        for (int i = 3; i < argc; ++i)
            status |= stopIn(argv[1], argv[i]) ? 0 : 1;
        return status;
    }
    if (argc == 6 && std::strcmp(argv[3], "roundtrip") == 0)
        return roundtrip(argv[1], argv[2], argv[4], argv[5]);
    if (argc == 4 && std::strcmp(argv[3], "outage") == 0)
        return outage(argv[1], argv[2]);
    if (argc == 6 && std::strcmp(argv[3], "soak") == 0)
        return soak(argv[1], argv[2], std::atoi(argv[4]), std::atoi(argv[5]));
    if (argc == 4 && std::strcmp(argv[3], "older_server") == 0)
        return olderServer(argv[1], argv[2]);
    if (argc == 4 && std::strcmp(argv[3], "restart") == 0)
        return restart(argv[1], argv[2]);
    if (argc == 5 && std::strcmp(argv[3], "flush") == 0)
        return flush(argv[1], argv[2], argv[4]);
    if (argc == 5 && std::strcmp(argv[3], "secret") == 0) {
        return secret(argv[1], argv[2], argv[4]);
    }
    if (argc == 5 && std::strcmp(argv[3], "friends") == 0) {
        return friendsFromServer(argv[1], argv[2], argv[4]);
    }
    if ((argc == 5 || argc == 6) && std::strcmp(argv[3], "signin") == 0) {
        return signin(argv[1], argv[2], argv[4], argc == 6 && std::strcmp(argv[5], "leaky") == 0);
    }
    std::fprintf(stderr, "usage: link_e2e <nexus-server> <dir> roundtrip <host> <nexus-fake-driver> | outage | "
                         "soak <clients> <prompts> | restart | older_server | flush <nexus-fake-driver> |\n"
                         "       secret <nexus-fake-driver> | friends <nexus-fake-driver> | signin <fake claude> [leaky]\n"
                         "       link_e2e <nexus-server> stop <dir>...\n");
    return 2;
}
