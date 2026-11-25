#include "link/client.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>

#include "link/mosh_link.h"
#include "sync/redial.h"

namespace nexus::link {

namespace {

// The longest the thread sleeps: the status line counts seconds without contact.
constexpr int kMaxWaitMs = 250;
// How long the shutdown waits for the server's acknowledgement before giving up.
constexpr auto kShutdownWait = std::chrono::seconds(2);

void drain(int fd) {
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0) {
    }
}

// "window-" and 16 random hex digits: a client id (server/server.h validClientId) no other window or launch has.
std::string makeClientId() {
    std::random_device device;
    const std::uint64_t bits = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(bits));
    return std::string("window-") + hex;
}

} // namespace

ClientLink::ClientLink(Options options) : options_(std::move(options)) {
    int fds[2];
    if (pipe(fds) == 0) {
        wakeRead_ = fds[0];
        wakeWrite_ = fds[1];
        fcntl(wakeRead_, F_SETFL, O_NONBLOCK);
        fcntl(wakeWrite_, F_SETFL, O_NONBLOCK);
    }
    thread_ = std::thread([this] { run(); });
}

ClientLink::~ClientLink() {
    stop_ = true;
    nudge();
    thread_.join();
    if (wakeRead_ >= 0)
        close(wakeRead_);
    if (wakeWrite_ >= 0)
        close(wakeWrite_);
}

void ClientLink::nudge() {
    if (wakeWrite_ >= 0) {
        const char c = 1;
        [[maybe_unused]] ssize_t n = write(wakeWrite_, &c, 1);
    }
}

std::uint64_t ClientLink::send(sync::Action action) {
    std::uint64_t number;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(action));
        number = ++sent_;
        snapshot_.sent = sent_;
        snapshot_.acknowledged = false;
    }
    nudge();
    return number;
}

bool ClientLink::poll(Snapshot* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!changed_)
        return false;
    *out = snapshot_;
    changed_ = false;
    return true;
}

void ClientLink::publish(const Snapshot& s) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // s.sent counts the actions the session holds; sent_ those handed to the link, some maybe not yet moved in.
        snapshot_ = s;
        snapshot_.sent = sent_;
        snapshot_.acknowledged = s.acknowledged && s.sent == sent_;
        changed_ = true;
    }
    if (options_.wake)
        options_.wake();
}

void ClientLink::run() {
    Snapshot s;
    s.phase = sync::LinkPhase::Connecting;
    publish(s);
    const std::string host = options_.bootstrap.host;
    const std::string client = options_.client.empty() ? makeClientId() : options_.client;
    sync::Redial redial;
    std::unique_ptr<ClientSession> session;
    bool refused = false;       // a server whose format this window no longer reads: no attempt after it
    bool failedLast = false;    // the latest attempt failed: the phase says so, with why
    bool lostLogged = false;
    std::uint64_t handed = 0;   // actions moved into a session (each once: one sent again is not counted again)
    int lastUnknown = 0;
    std::string why;

    // One run of the bootstrap: a new session, which goes on from the first action the one it replaces has no
    // acknowledgement for, holding those again; or false, with why, and the session held kept.
    auto dial = [&](std::string* problem) -> bool {
        std::string ip = options_.ip;
        if (ip.empty() && !resolveHost(options_.bootstrap, runCommand, &ip, problem)) {
            return false;
        }
        Bootstrap b = options_.bootstrap;
        b.client = client;
        b.next = session ? session->firstUnacknowledged() : 1;
        b.askRunning = !redial.mayStart();
        Connect connect;
        if (!bootstrap(b, runCommand, &connect, problem)) {
            return false;
        }
        // The server's Hello, against the window's (sync/formats.h): a server whose proto is below the window's
        // min_peer is refused in one plain line, never a link that cannot be read. A server from before Hello states
        // only its format. The link uses the format the server answered with, which is never newer than the one the
        // window stated.
        sync::Hello mine;
        if (!statesHello(b, &mine)) {
            mine = sync::statedFormat(b.stateFormat);
        }
        const std::string refusal = sync::refusalOfServer(connect.hello, mine);
        if (!refusal.empty()) {
            connect.key.assign(connect.key.size(), '\0');
            refused = true;
            *problem = refusal;
            if (session) {
                std::fprintf(stderr, "nexus: link to %s: %s\n", host.c_str(), refusal.c_str());
            }
            return false;
        }
        freezeClock();
        std::unique_ptr<ClientSession> made;
        try {
            made = std::make_unique<ClientSession>(connect.key, ip, connect.port, b.next, connect.format);
        } catch (const std::exception& e) {
            connect.key.assign(connect.key.size(), '\0');
            *problem = e.what();
            return false;
        }
        connect.key.assign(connect.key.size(), '\0');
        if (session) {
            std::vector<sync::Action> unacked = session->unacknowledged();
            for (sync::Action& a : unacked) {
                made->add(std::move(a));
            }
            ++s.reconnects;
            lastUnknown = 0;
            std::fprintf(stderr, "nexus: link to %s: a new session, from action %llu\n", host.c_str(),
                static_cast<unsigned long long>(b.next));
        }
        s.loopback = ip == "127.0.0.1" || ip == "::1";
        session = std::move(made);   // the old one goes: its server ended it, or ends it for this one
        return true;
    };

    std::string lastStatus;
    std::uint64_t lastNum = ~std::uint64_t(0);
    bool lastAcknowledged = false;
    std::uint64_t lastHanded = 0;
    std::uint64_t lastReconnects = 0;
    sync::LinkPhase lastPhase = s.phase;
    while (!stop_) {
        freezeClock();
        if (!refused && redial.due(clockMs())) {
            if (session && !lostLogged) {
                std::fprintf(stderr, "nexus: link to %s: %s; running the bootstrap again\n", host.c_str(),
                    redial.serverEnded() ? "the server ended the session" : "the server has not been heard");
                lostLogged = true;
            }
            std::string problem;
            const bool dialed = dial(&problem);
            freezeClock();
            if (dialed) {
                redial.dialed(clockMs());
                failedLast = false;
                s.problem.clear();
            } else {
                redial.failed(clockMs());
                failedLast = true;
                s.problem = problem;
            }
        }
        if (session) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (sync::Action& a : pending_) {
                session->add(std::move(a));
                ++handed;
            }
            pending_.clear();
        }
        if (session && !redial.serverEnded()) {
            if (!session->tick(&why)) {
                std::fprintf(stderr, "nexus: link to %s: %s\n", host.c_str(), why.c_str());
            }
            if (session->serverEnded()) {
                redial.ended();
            }
        }
        std::vector<pollfd> fds;
        fds.push_back(pollfd{wakeRead_, POLLIN, 0});
        int wait = kMaxWaitMs;
        if (session && !redial.serverEnded()) {
            for (int fd : session->fds()) {
                fds.push_back(pollfd{fd, POLLIN, 0});
            }
            wait = std::min(wait, std::max(0, session->waitMs()));
        }
        if (!refused) {
            wait = static_cast<int>(std::min<sync::RedialMs>(static_cast<sync::RedialMs>(wait),
                redial.waitMs(clockMs())));
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), wait);
        freezeClock();
        if (fds[0].revents & POLLIN) {
            drain(wakeRead_);
        }
        bool received = false;
        for (std::size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents & POLLIN) {
                received = true;
                if (!session->receive(&why)) {
                    std::fprintf(stderr, "nexus: link to %s: %s\n", host.c_str(), why.c_str());
                }
            }
        }
        if (received && session && !redial.serverEnded() && !session->tick(&why)) {
            std::fprintf(stderr, "nexus: link to %s: %s\n", host.c_str(), why.c_str());
        }
        if (session) {
            if (session->serverEnded()) {
                redial.ended();
            }
            if (!redial.serverEnded() && session->stateNum() > 0) {
                redial.heard(session->heardAtMs());
            }
        }
        const sync::RedialMs now = clockMs();
        const bool live = session && session->stateNum() > 0 && !redial.lost(now);
        if (live) {
            lostLogged = false;
        }
        // Connected once this session's server is heard and not lost; until then the phase says the link is on its way
        // (or why the latest attempt failed), and the sessions stay the last ones the window had.
        s.phase = live ? sync::LinkPhase::Connected :
            (failedLast ? sync::LinkPhase::Failed : sync::LinkPhase::Connecting);
        s.heardMsAgo = live ? session->heardMsAgo() : 0;
        s.stateNum = session ? session->stateNum() : 0;
        s.sent = handed;
        s.acknowledged = session ? session->allAcknowledged() : handed == 0;
        const std::string status = sync::linkStatus(s.phase, host, s.heardMsAgo, s.problem);
        if (s.stateNum != lastNum || status != lastStatus || s.acknowledged != lastAcknowledged ||
                s.sent != lastHanded || s.reconnects != lastReconnects || s.phase != lastPhase) {
            if (live) {
                s.sessions = session->state();
                // What a later server's state held that this window skipped (sync/sessions.h): logged when it changes,
                // so a window that shows a notice for an unknown entry kind says why.
                if (s.sessions.unknown() != lastUnknown) {
                    lastUnknown = s.sessions.unknown();
                    if (lastUnknown > 0) {
                        std::fprintf(stderr, "nexus: link to %s: the server's state holds %d field(s) or value(s) this "
                            "window does not know (a later nexus-server): skipped; an entry of a kind it does not know "
                            "shows as a notice\n", host.c_str(), lastUnknown);
                    }
                }
            }
            lastHanded = s.sent;
            lastNum = s.stateNum;
            lastStatus = status;
            lastAcknowledged = s.acknowledged;
            lastReconnects = s.reconnects;
            lastPhase = s.phase;
            publish(s);
        }
    }
    // A session the server ended has nobody to acknowledge a shutdown.
    if (!session || redial.serverEnded()) {
        return;
    }
    session->shutdown();
    const auto deadline = std::chrono::steady_clock::now() + kShutdownWait;
    while (!session->shutdownDone() && std::chrono::steady_clock::now() < deadline) {
        freezeClock();
        session->tick(&why);
        std::vector<pollfd> fds;
        for (int fd : session->fds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::clamp(session->waitMs(), 0, kMaxWaitMs));
        freezeClock();
        for (const pollfd& p : fds) {
            if (p.revents & POLLIN) {
                session->receive(&why);
            }
        }
    }
}

} // namespace nexus::link
