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
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fake_mosh.h"
#include "friends.h"
#include "link/bootstrap.h"
#include "link/client.h"
#include "link/mosh_link.h"
#include "model.h"
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
        b.command = {binary, "new", "--dir", dir};
        return b;
    }
};

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
    CHECK(writeFile(dir + "/ada.script", "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\n"
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n"
        "send\nemit {\"kind\":\"session\",\"id\":\"s-1\"}\nemit {\"kind\":\"text\",\"delta\":\"hello \"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"back\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n"));
    CHECK(writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script\nfriend ada fake " + dir +
        "\n"));
    Model model(kFriends, true);
    sync::Mirror mirror;
    const auto start = Clock::now();
    {
        link::ClientLink::Options options;
        options.bootstrap = server.bootstrap(host);
        link::ClientLink link(options);
        link::ClientLink::Snapshot s;
        auto pump = [&] {
            if (link.poll(&s))
                mirror.apply(model, s.sessions);
        };
        CHECK(waitFor([&] { pump(); return s.phase != sync::LinkPhase::Connecting; }, 30));
        if (s.phase == sync::LinkPhase::Failed)
            std::fprintf(stderr, "link_e2e: %s\n", s.problem.c_str());
        CHECK(s.phase == sync::LinkPhase::Connected);
        std::printf("connected to %s in %.2f s\n", host.c_str(), seconds(start));
        const auto sent = Clock::now();
        for (const sync::Action& a : ensures()) {
            link.send(a);
        }
        link.send(act(sync::ActionKind::Send, sid("ada"), "hello over mosh"));
        link.send(act(sync::ActionKind::Queue, sid("bo"), "for later"));
        const std::size_t ada = tabOf(model, "ada");
        const std::size_t bo = tabOf(model, "bo");
        CHECK(waitFor([&] { pump(); return model.tab(ada).transcript.size() == 2 && model.tab(bo).queue.size() == 1; },
            10));
        std::printf("round trip in %.3f s\n", seconds(sent));
        CHECK(model.tab(ada).transcript.size() == 2);
        if (model.tab(ada).transcript.size() == 2) {
            CHECK(model.tab(ada).transcript[0].kind == EntryKind::Person);
            CHECK(model.tab(ada).transcript[0].text == "hello over mosh");
            CHECK(model.tab(ada).transcript[1].kind == EntryKind::Friend);
            CHECK(model.tab(ada).transcript[1].text == "hello back");
        }
        CHECK(model.tab(bo).queue == std::vector<std::string>{"for later"});
        CHECK(model.tab(bo).transcript.empty());
        CHECK(waitFor([&] { pump(); return s.acknowledged && s.sent == 5; }, 10));
        CHECK(sessionsAsMinted(s.sessions));
    }   // the link shuts down here
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
    auto settle = [&](int seconds, const std::function<bool()>& done) {
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        while (Clock::now() < deadline && !done())
            step(windows, &relay, 50);
        return done();
    };
    CHECK(settle(10, [&] { return w.session->stateNum() > 0; }));
    for (const sync::Action& a : ensures()) {
        w.session->add(a);
    }
    w.session->add(act(sync::ActionKind::Send, sid("ada"), "before"));
    CHECK(settle(10, [&] { return w.model.tab(ada).transcript.size() == 2 && w.session->allAcknowledged(); }));

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
    CHECK(settle(15, [&] { return w.model.tab(ada).transcript.size() == 8 && w.session->allAcknowledged(); }));
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
    CHECK(settle(10, [&] { return w.model.tab(ada).queue.size() == 1 && w.session->allAcknowledged(); }));
    CHECK(w.model.tab(ada).transcript.size() == 8);
    w.session->shutdown();
    settle(3, [&] { return w.session->shutdownDone(); });
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
        if (rng.chance(4))
            step(windows, nullptr, 0);
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
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < deadline && !converged())
        step(windows, nullptr, 20);
    CHECK(converged());
    std::printf("%d prompts from %d windows applied and seen by every window in %.2f s\n", prompts, clients,
        seconds(sending));
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

// `nexus-server serve --dir <dir>` as this test's child, its output to the server's log, so the test can kill it.
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
    pid_t pid = -1;
    if (posix_spawn(&pid, binary.c_str(), &files, nullptr, argv.data(), environ) != 0)
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
    CHECK(waitFor([&] { return answering(dir); }, 10));
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
    auto settle = [](Window& w, int seconds, const std::function<bool()>& done) {
        std::vector<Window*> ws{&w};
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        while (Clock::now() < deadline && !done())
            step(ws, nullptr, 20);
        return done();
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
        CHECK(settle(w, 10, [&] { return w.session->allAcknowledged() && adaTexts(w.session->state()).size() == 4; }));
        // The fourth is sent and the server killed at once: applied and durable, or not applied at all.
        add(w, before[3]);
        settle(w, 1, [&] { return false; });
    }
    CHECK(kill(pid, SIGKILL) == 0);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    std::printf("server killed with SIGKILL\n");

    pid = startServe(binary, dir);
    CHECK(pid > 0);
    CHECK(waitFor([&] { return answering(dir); }, 10));
    Window w;
    if (!session(1, &w))
        return 1;
    // The fresh sync: what was acknowledged before the kill is there before the window sends anything.
    CHECK(settle(w, 10, [&] { return w.session->stateNum() > 0 && adaTexts(w.session->state()).size() >= 4; }));
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
    CHECK(settle(w, 15, [&] { return w.session->allAcknowledged() && adaTexts(w.session->state()) == want; }));
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
    settle(w, 3, [&] { return w.session->shutdownDone(); });
    std::printf("restart: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;   // the Server stops the restarted server
}

} // namespace

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
    if (argc == 4 && std::strcmp(argv[3], "restart") == 0)
        return restart(argv[1], argv[2]);
    std::fprintf(stderr, "usage: link_e2e <nexus-server> <dir> roundtrip <host> <nexus-fake-driver> | outage | "
                         "soak <clients> <prompts> | restart\n"
                         "       link_e2e <nexus-server> stop <dir>...\n");
    return 2;
}
