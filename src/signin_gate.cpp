#include "signin_gate.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace nexus {

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
using SockLen = int;
constexpr Socket kNoSocket = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
int pollFds(PollFd* fds, std::size_t n, int ms) { return WSAPoll(fds, static_cast<ULONG>(n), ms); }
void closeSocket(Socket s) { closesocket(s); }
int lastError() { return WSAGetLastError(); }
std::string errorText(int e) { return "error " + std::to_string(e); }
bool nonBlocking(Socket s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
long sendSome(Socket s, const char* p, std::size_t n) { return send(s, p, static_cast<int>(n), 0); }
long recvSome(Socket s, char* p, std::size_t n) { return recv(s, p, static_cast<int>(n), 0); }
// Winsock, once per process; never cleaned up, as the process's other users of it may still hold sockets.
bool startSockets() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
using Socket = int;
using SockLen = socklen_t;
constexpr Socket kNoSocket = -1;
using PollFd = pollfd;
int pollFds(PollFd* fds, std::size_t n, int ms) { return poll(fds, static_cast<nfds_t>(n), ms); }
void closeSocket(Socket s) { close(s); }
int lastError() { return errno; }
std::string errorText(int e) { return std::strerror(e); }
bool nonBlocking(Socket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(s, F_SETFD, FD_CLOEXEC) == 0;
}
long sendSome(Socket s, const char* p, std::size_t n) {
#if defined(MSG_NOSIGNAL)
    return static_cast<long>(send(s, p, n, MSG_NOSIGNAL));
#else
    return static_cast<long>(send(s, p, n, 0));
#endif
}
long recvSome(Socket s, char* p, std::size_t n) { return static_cast<long>(recv(s, p, n, 0)); }
bool startSockets() { return true; }
#endif

// Whether `e` says this machine has no such address family or loopback address at all (and so no browser reaches it).
bool noSuchFamily(int e) {
#if defined(_WIN32)
    return e == WSAEAFNOSUPPORT || e == WSAEADDRNOTAVAIL || e == WSAEPROTONOSUPPORT || e == WSAEPFNOSUPPORT;
#else
    return e == EAFNOSUPPORT || e == EADDRNOTAVAIL || e == EPROTONOSUPPORT || e == EPFNOSUPPORT;
#endif
}

// Listens on `port` at the loopback address of `family`; kNoSocket, with why and the error, when it cannot.
Socket listenOn(int family, std::uint16_t port, std::string* why, int* error) {
    *error = 0;
    const Socket s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        *error = lastError();
        *why = "socket: " + errorText(*error);
        return kNoSocket;
    }
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    }
#endif
    int rc = 0;
    if (family == AF_INET6) {
        int on = 1;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof on);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        a.sin6_addr = in6addr_loopback;
        rc = bind(s, reinterpret_cast<const sockaddr*>(&a), static_cast<SockLen>(sizeof a));
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        rc = bind(s, reinterpret_cast<const sockaddr*>(&a), static_cast<SockLen>(sizeof a));
    }
    if (rc != 0 || listen(s, 8) != 0 || !nonBlocking(s)) {
        *error = lastError();
        *why = "port " + std::to_string(port) + ": " + errorText(*error);
        closeSocket(s);
        return kNoSocket;
    }
    return s;
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Connection {
    Socket fd = kNoSocket;
    std::string bytes;      // what it sent so far; wiped as it closes
    std::size_t slot = 0;   // its sync::GateSlots slot, which gives its deadline
};

// Writes `bytes` whole, waiting on the socket for at most a second in all: the answer is a few hundred bytes to a
// loopback peer, so this is a formality; a peer that does not read loses its answer.
void answer(Socket s, const std::string& bytes) {
    std::size_t done = 0;
    for (int tries = 0; done < bytes.size() && tries < 10; ++tries) {
        const long n = sendSome(s, bytes.data() + done, bytes.size() - done);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
            continue;
        }
        PollFd p{};
        p.fd = s;
        p.events = POLLOUT;
        pollFds(&p, 1, 100);
    }
}

void drop(Connection& c) {
    closeSocket(c.fd);
    c.fd = kNoSocket;
    sync::wipeBytes(c.bytes);
}

} // namespace

struct SignInGate::Shared {
    std::mutex mutex;
    sync::SecretValue query;    // with taken, until take()
    bool taken = false;
    bool failed = false;
    std::string why;
    std::atomic<bool> stop{false};
};

namespace {

void serveGate(const std::shared_ptr<SignInGate::Shared>& shared, sync::SignInTarget target) {
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->failed = true;
        shared->why = why;
    };
    if (!startSockets()) {
        fail("the sockets could not be started");
        return;
    }
    std::string why;
    int error = 0;
    std::vector<Socket> listeners;
    const Socket v4 = listenOn(AF_INET, target.port, &why, &error);
    if (v4 == kNoSocket) {
        fail("the sign-in's callback cannot be received on this machine: " + why);
        return;
    }
    listeners.push_back(v4);
    // [::1] too: a browser may send `localhost` there first, and a port another program holds there would take the
    // callback. Bound, or the gate fails; only a machine with no IPv6 loopback at all goes on with 127.0.0.1 alone.
    const Socket v6 = listenOn(AF_INET6, target.port, &why, &error);
    if (v6 != kNoSocket) {
        listeners.push_back(v6);
    } else if (!noSuchFamily(error)) {
        closeSocket(v4);
        fail("the sign-in's callback cannot be received on this machine: [::1] " + why);
        return;
    }
    sync::CallbackGate gate(std::move(target));
    sync::GateSlots slots;
    std::vector<Connection> connections;
    std::vector<PollFd> fds;
    bool done = false;
    while (!done && !shared->stop.load()) {
        fds.clear();
        for (const Socket l : listeners) {
            PollFd p{};
            p.fd = l;
            p.events = POLLIN;
            fds.push_back(p);
        }
        for (const Connection& c : connections) {
            PollFd p{};
            p.fd = c.fd;
            p.events = POLLIN;
            fds.push_back(p);
        }
        if (pollFds(fds.data(), fds.size(), SignInGate::kPollMs) < 0) {
#if !defined(_WIN32)
            if (errno == EINTR) {
                continue;
            }
#endif
            fail("the sign-in's callback stopped being received: " + errorText(lastError()));
            break;
        }
        const std::uint64_t now = nowMs();
        // Each connection has its time from its accept, whatever it sent meanwhile (sync::GateSlots).
        for (const std::size_t slot : slots.expire(now)) {
            for (Connection& c : connections) {
                if (c.fd != kNoSocket && c.slot == slot) {
                    drop(c);
                }
            }
        }
        for (std::size_t i = 0; i < listeners.size(); ++i) {
            if ((fds[i].revents & POLLIN) == 0) {
                continue;
            }
            const Socket s = accept(listeners[i], nullptr, nullptr);
            if (s == kNoSocket) {
                continue;
            }
            std::size_t slot = 0;
            if (!nonBlocking(s) || !slots.accept(now, &slot)) {
                closeSocket(s);
                continue;
            }
            connections.push_back(Connection{s, std::string(), slot});
        }
        // The connections polled this round are the first `polled`; any accepted above wait for the next.
        const std::size_t polled = fds.size() - listeners.size();
        for (std::size_t i = 0; i < polled && !done; ++i) {
            Connection& c = connections[i];
            const PollFd& p = fds[listeners.size() + i];
            if (c.fd == kNoSocket) {
                continue;   // its time ran out above
            }
            if ((p.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                char buf[2048];
                const long n = recvSome(c.fd, buf, sizeof buf);
                if (n <= 0) {
                    slots.release(c.slot);
                    drop(c);
                    continue;
                }
                c.bytes.append(buf, static_cast<std::size_t>(n));
                std::memset(buf, 0, sizeof buf);
                sync::SecretValue query;
                std::string response;
                const sync::CallbackGate::Verdict v = gate.take(c.bytes, &query, &response);
                if (v == sync::CallbackGate::Verdict::More) {
                    continue;
                }
                if (v == sync::CallbackGate::Verdict::Take) {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    if (shared->stop.load()) {
                        response = "HTTP/1.1 410 Gone\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    } else {
                        shared->query = std::move(query);
                        shared->taken = true;
                    }
                    done = true;
                }
                answer(c.fd, response);
                slots.release(c.slot);
                drop(c);
            }
        }
        std::erase_if(connections, [](const Connection& c) { return c.fd == kNoSocket; });
    }
    for (Connection& c : connections) {
        drop(c);
    }
    for (const Socket l : listeners) {
        closeSocket(l);
    }
}

} // namespace

std::string localHostName() {
#if defined(_WIN32)
    char name[256] = {};
    DWORD size = sizeof name;
    if (GetComputerNameExA(ComputerNameDnsHostname, name, &size) == 0) {
        return std::string();
    }
    return std::string(name, size);
#else
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) != 0) {
        return std::string();
    }
    return std::string(name);
#endif
}

std::string newClaimToken() {
    static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device random;
    std::string token;
    while (token.size() < 32) {
        const unsigned v = random();
        // 248 = 4 * 62: the bytes past it are dropped, so that each letter is as likely as any other.
        const unsigned b = v & 0xffU;
        if (b < 248U) {
            token.push_back(kAlphabet[b % 62U]);
        }
    }
    return token;
}

SignInGate::SignInGate(sync::SignInTarget target) : shared_(std::make_shared<Shared>()) {
    // Detached: the thread holds the shared state, and ends within kPollMs of being asked to, so no frame waits on it.
    std::thread([shared = shared_, t = std::move(target)]() mutable { serveGate(shared, std::move(t)); }).detach();
}

SignInGate::~SignInGate() {
    shared_->stop.store(true);
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->query.clear();
}

bool SignInGate::take(sync::SecretValue* query) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (!shared_->taken) {
        return false;
    }
    shared_->taken = false;
    *query = std::move(shared_->query);
    shared_->query.clear();
    return true;
}

bool SignInGate::failed(std::string* why) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->failed) {
        *why = shared_->why;
    }
    return shared_->failed;
}

bool SignInGate::stopAndTake(sync::SecretValue* query) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stop.store(true);
    if (!shared_->taken) return false;
    shared_->taken = false;
    *query = std::move(shared_->query);
    return true;
}

} // namespace nexus
