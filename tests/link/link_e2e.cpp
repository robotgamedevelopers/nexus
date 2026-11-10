// Functional tests of the link, end to end: nexus-server started over real ssh to this machine by the window's own
// bootstrap, and headless windows (the window's model and mirror, without drawing) talking to it over real UDP through
// mosh's transport. Each run has a server of its own, in the directory <dir> it is given (its socket and log), and
// stops it at the end. A run stops any server an earlier run left in <dir> first, and the `stop` form (the tests'
// cleanup fixture, which ctest runs even after a crash or a timeout) stops the servers in every directory it names, so
// no server outlives the tests. The session key is never printed.
//
//   link_e2e <nexus-server> <dir> roundtrip <host>
//       the window's own link thread (link/client.h) through ssh <host>: a prompt sent to one friend comes back with
//       its reply, one queued to another shows queued; then the link shuts down and the server ends the session
//   link_e2e <nexus-server> <dir> outage
//       through a relay on loopback that the test can cut: prompts sent while the network is down for 8 s arrive once
//       each when it comes back, the window says it has no contact while it is down, and converges after
//   link_e2e <nexus-server> <dir> soak <clients> <prompts>
//       several windows at once, prompts from each to random friends: every window ends holding the server's state,
//       each prompt applied once, each window's prompts in its order, and every window's transcripts agree
//   link_e2e <nexus-server> stop <dir>...
//       stops the server in each directory, if one runs, and removes the directory

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ---- roundtrip: the window's own link thread ----

int roundtrip(const std::string& binary, const std::string& dir, const std::string& host) {
    Server server(binary, dir);
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
                mirror.apply(model, s.tabs);
        };
        CHECK(waitFor([&] { pump(); return s.phase != sync::LinkPhase::Connecting; }, 30));
        if (s.phase == sync::LinkPhase::Failed)
            std::fprintf(stderr, "link_e2e: %s\n", s.problem.c_str());
        CHECK(s.phase == sync::LinkPhase::Connected);
        std::printf("connected to %s in %.2f s\n", host.c_str(), seconds(start));
        const auto sent = Clock::now();
        link.send(sync::ActionKind::Send, "ada", "hello over mosh");
        link.send(sync::ActionKind::Queue, "bo", "for later");
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
            CHECK(model.tab(ada).transcript[1].text == sync::kReceived);
        }
        CHECK(model.tab(bo).queue == std::vector<std::string>{"for later"});
        CHECK(model.tab(bo).transcript.empty());
        CHECK(waitFor([&] { pump(); return s.acknowledged && s.sent == 2; }, 10));
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
    w.session->add(sync::ActionKind::Send, "ada", "before");
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
            w.session->add(sync::ActionKind::Send, "ada", "during " + std::to_string(sentDuring));
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
    const std::vector<std::string> want = {"before", sync::kReceived, "during 0", sync::kReceived, "during 1",
        sync::kReceived, "during 2", sync::kReceived};
    CHECK(t.size() == want.size());
    for (std::size_t i = 0; i < t.size() && i < want.size(); ++i)
        CHECK(t[i].text == want[i]);
    const sync::TabView* v = w.session->state().find("ada");
    CHECK(v != nullptr && v->total() == 8);

    // A second, shorter cut, with a prompt queued in it.
    relay.down = true;
    const auto cut2 = Clock::now();
    w.session->add(sync::ActionKind::Queue, "ada", "queued while down");
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

// The entries a window shows, by the server's transcript index, with the entries it never received left empty.
std::vector<const Entry*> byIndex(const Tab& tab) {
    std::vector<const Entry*> out;
    for (const Entry& e : tab.transcript) {
        if (e.kind == EntryKind::Notice) {
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
            windows[c]->session->add(sync::ActionKind::Queue, tab, text);
            ++queues[tab];
        } else {
            windows[c]->session->add(sync::ActionKind::Send, tab, text);
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
                const sync::TabView* v = w->session->state().find(f.name);
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
                    CHECK(e->kind == EntryKind::Friend && e->text == sync::kReceived);
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

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[2], "stop") == 0) {
        int status = 0;
        for (int i = 3; i < argc; ++i)
            status |= stopIn(argv[1], argv[i]) ? 0 : 1;
        return status;
    }
    if (argc == 5 && std::strcmp(argv[3], "roundtrip") == 0)
        return roundtrip(argv[1], argv[2], argv[4]);
    if (argc == 4 && std::strcmp(argv[3], "outage") == 0)
        return outage(argv[1], argv[2]);
    if (argc == 6 && std::strcmp(argv[3], "soak") == 0)
        return soak(argv[1], argv[2], std::atoi(argv[4]), std::atoi(argv[5]));
    std::fprintf(stderr, "usage: link_e2e <nexus-server> <dir> roundtrip <host> | outage | soak <clients> <prompts>\n"
                         "       link_e2e <nexus-server> stop <dir>...\n");
    return 2;
}
