#include "link/client.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>

#include "link/mosh_link.h"

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

void ClientLink::send(sync::Action action) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(action));
        ++sent_;
        snapshot_.sent = sent_;
        snapshot_.acknowledged = false;
    }
    nudge();
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
    auto fail = [&](const std::string& why) {
        s.phase = sync::LinkPhase::Failed;
        s.problem = why;
        publish(s);
    };
    std::string ip = options_.ip;
    std::string why;
    if (ip.empty() && !resolveHost(options_.bootstrap, runCommand, &ip, &why))
        return fail(why);
    Connect connect;
    if (!bootstrap(options_.bootstrap, runCommand, &connect, &why))
        return fail(why);
    // A server whose format this window no longer reads: one plain line, at once, never a link that cannot be read.
    // The link uses the format the server answered with, which is never newer than the one the window stated.
    const std::string refused = sync::serverRefusal(connect.format);
    if (!refused.empty()) {
        connect.key.assign(connect.key.size(), '\0');
        return fail(refused);
    }
    std::unique_ptr<ClientSession> session;
    try {
        session = std::make_unique<ClientSession>(connect.key, ip, connect.port, 1, connect.format);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    connect.key.assign(connect.key.size(), '\0');

    std::uint64_t handed = 0;       // actions moved into the session
    std::string lastStatus;
    std::uint64_t lastNum = ~std::uint64_t(0);
    bool lastAcknowledged = false;
    std::uint64_t lastHanded = 0;
    while (!stop_) {
        freezeClock();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (sync::Action& a : pending_) {
                session->add(std::move(a));
                ++handed;
            }
            pending_.clear();
        }
        if (!session->tick(&why))
            std::fprintf(stderr, "nexus: link to %s: %s\n", options_.bootstrap.host.c_str(), why.c_str());
        std::vector<pollfd> fds;
        fds.push_back(pollfd{wakeRead_, POLLIN, 0});
        for (int fd : session->fds())
            fds.push_back(pollfd{fd, POLLIN, 0});
        const int wait = std::clamp(session->waitMs(), 0, kMaxWaitMs);
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), wait);
        freezeClock();
        if (fds[0].revents & POLLIN)
            drain(wakeRead_);
        for (std::size_t i = 1; i < fds.size(); ++i) {
            if ((fds[i].revents & POLLIN) && !session->receive(&why))
                std::fprintf(stderr, "nexus: link to %s: %s\n", options_.bootstrap.host.c_str(), why.c_str());
        }
        s.phase = session->stateNum() > 0 ? sync::LinkPhase::Connected : sync::LinkPhase::Connecting;
        s.heardMsAgo = session->heardMsAgo();
        s.stateNum = session->stateNum();
        s.sent = handed;
        s.acknowledged = session->allAcknowledged();
        const std::string status = sync::linkStatus(s.phase, options_.bootstrap.host, s.heardMsAgo, s.problem);
        if (s.stateNum != lastNum || status != lastStatus || s.acknowledged != lastAcknowledged ||
                s.sent != lastHanded) {
            lastHanded = s.sent;
            s.sessions = session->state();
            lastNum = s.stateNum;
            lastStatus = status;
            lastAcknowledged = s.acknowledged;
            publish(s);
        }
    }
    session->shutdown();
    const auto deadline = std::chrono::steady_clock::now() + kShutdownWait;
    while (!session->shutdownDone() && std::chrono::steady_clock::now() < deadline) {
        freezeClock();
        session->tick(&why);
        std::vector<pollfd> fds;
        for (int fd : session->fds())
            fds.push_back(pollfd{fd, POLLIN, 0});
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::clamp(session->waitMs(), 0, kMaxWaitMs));
        freezeClock();
        for (const pollfd& p : fds) {
            if (p.revents & POLLIN)
                session->receive(&why);
        }
    }
}

} // namespace nexus::link
