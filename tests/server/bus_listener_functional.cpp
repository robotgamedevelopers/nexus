// The production receiver, launched by a fake nova-bus hook. Only a zero exit from the receiver lets the fake bus
// acknowledge, just as it would on Redis. Each run owns its tools, child processes and receipts.
#include "server/bus_listener.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using nexus::server::BusAccept;
using nexus::server::BusListeners;
using nexus::server::BusMessage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const fs::path& path, const std::string& data, bool executable = false) {
    std::ofstream file(path, std::ios::binary);
    file << data;
    file.close();
    require(file.good(), "could not write test fixture");
    if (executable) fs::permissions(path, fs::perms::owner_all);
}

struct Home {
    fs::path root;
    Home() {
        char pattern[] = "/tmp/nexus-bus-test-XXXXXX";
        require(::mkdtemp(pattern) != nullptr, "could not create test home");
        root = pattern;
        const auto bin = root / ".local/bin";
        fs::create_directories(bin);
        writeFile(bin / "nova-secrets", R"SH(#!/bin/sh
set -eu
[ "$1" = exec ] && [ "$2" = --only ] && [ "$3" = NOVA_BUS_PASSWORD ] && [ "$4" = -- ] || exit 71
shift 4
NOVA_BUS_PASSWORD=fixture-only; export NOVA_BUS_PASSWORD
exec "$@"
)SH", true);
        writeFile(bin / "nova-bus", R"SH(#!/bin/sh
set -eu
[ "$1" = hook ] && [ "$NOVA_BUS_USER" = bus ] && [ "$NOVA_BUS_PASSWORD" = fixture-only ] &&
    [ "$NOVA_BUS_REDIS" = 127.0.0.1:6379 ] || exit 72
shift
while [ $# -gt 0 ]; do
    case "$1" in
        --as) inbox=$2 ;;
        --exec) receiver=$2 ;;
        --harness) [ "$2" = claude ] || exit 73 ;;
        *) exit 74 ;;
    esac
    shift 2
done
trap ': > "$HOME/$inbox.term"; exit 0' TERM
printf '%s\n' "$$" > "$HOME/$inbox.pid"
printf '%s\n' "$$" >> "$HOME/$inbox.launches"
printf '%s\n' "$receiver" > "$HOME/$inbox.receiver"
printf 'HOOK OK name=%s\n' "$inbox" >&2
if [ "$inbox" = offline ]; then
    for attempt in 1 2; do
        printf 'nova-bus hook: redis at %s answered with an error; redis: %s; reconnecting\n' \
            "$NOVA_BUS_REDIS" "$NOVA_BUS_PASSWORD" >&2
    done
    # Block in the shell itself. A foreground child's startup can race a group SIGTERM
    # and leave the shell deferring its trap until the child exits.
    mkfifo "$HOME/$inbox.wait"
    exec 3<> "$HOME/$inbox.wait"
    : > "$HOME/$inbox.reported"
    IFS= read -r unused <&3
    exit 75
fi
if /bin/sh -c "$receiver" < "$HOME/$inbox.wire"; then
    : > "$HOME/$inbox.acked"
    exec /bin/sleep 30
fi
: > "$HOME/$inbox.refused"
exit 1
)SH", true);
    }
    ~Home() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

template<class Predicate, class Accept>
void until(BusListeners& listeners, Predicate done, Accept accept, const char* timeout) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, timeout);
        auto fds = listeners.fds();
        const int wait = listeners.waitMs();
        const auto result = ::poll(fds.data(), fds.size(), wait < 0 ? 25 : std::min(wait, 25));
        require(result >= 0 || errno == EINTR, "poll failed");
        listeners.serve(accept);
    }
}

void exercise(const fs::path& executable) {
    Home home;
    // The hook passes the helper invocation through sh -c. Real install paths can contain shell punctuation.
    const auto self = home.root / "test's nexus server";
    fs::create_symlink(executable, self);
    std::vector<std::string> reports;
    BusListeners listeners(home.root.string(), self.string(), [&](const std::string& message) {
        require(message.find("fixture-only") == std::string::npos, "hook diagnostic disclosed the fake credential");
        reports.push_back(message);
    });
    writeFile(home.root / "reader.wire", "1730800000000-1\tsender\t1730800000000-0\ttopic\\tname\tfirst\\nsecond\\r\\\\end\n");
    listeners.configure({{"reader", "127.0.0.1:6379"}});

    int offers = 0;
    bool ready = false, admitted = false;
    const auto accept = [&](const std::string& inbox, const BusMessage& message) {
        require(inbox == "reader", "delivery arrived at the wrong inbox");
        require(message.id == "1730800000000-1" && message.from == "sender" &&
                message.replyTo == "1730800000000-0", "message identity or reply reference changed");
        require(message.subject == "topic\tname" && message.body == "first\nsecond\r\\end", "TSV escapes lost");
        if (++offers == 1) return BusAccept::Retry;
        if (!ready) return BusAccept::Wait;
        admitted = true;
        return BusAccept::Admit;
    };
    until(listeners, [&] { return fs::exists(home.root / "reader.refused"); }, accept, "refused delivery was acknowledged or hung");
    until(listeners, [&] { return offers >= 3; }, accept, "listener failed to restart or hold a deferred message");
    std::ifstream launches(home.root / "reader.launches");
    int first = 0, second = 0, extra = 0;
    require((launches >> first >> second) && !(launches >> extra) && first != second,
            "deferred delivery restarted the hook instead of retaining its socket");
    require(!fs::exists(home.root / "reader.acked"), "deferred delivery was acknowledged");
    ready = true;
    until(listeners, [&] { return admitted; }, accept, "listener did not admit the deferred message");
    const int admittedAt = offers;
    const auto holdUntil = std::chrono::steady_clock::now() + 100ms;
    until(listeners, [&] { return std::chrono::steady_clock::now() >= holdUntil; }, accept, "failed to hold delivery");
    require(offers == admittedAt && !fs::exists(home.root / "reader.acked"), "hook acknowledged before store flush");
    listeners.flushed();
    until(listeners, [&] { return fs::exists(home.root / "reader.acked"); }, accept, "hook failed to acknowledge after store flush");

    // The socket is private even from a same-user process that knows the exact command and message.
    std::string receiver;
    std::ifstream command(home.root / "reader.receiver");
    require(bool(std::getline(command, receiver)), "missing receiver command");
    const int wire = ::open((home.root / "reader.wire").c_str(), O_RDONLY);
    require(wire >= 0, "could not open spoofed message");
    const pid_t stranger = ::fork();
    if (stranger == 0) {
        ::alarm(4);
        if (::dup2(wire, STDIN_FILENO) < 0) _exit(126);
        ::close(wire);
        ::execl("/bin/sh", "sh", "-c", receiver.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(wire);
    require(stranger > 0, "could not start unrelated helper");
    int strangerStatus = 0;
    until(listeners, [&] { return ::waitpid(stranger, &strangerStatus, WNOHANG) == stranger; },
          accept, "unrelated helper did not exit");
    require(offers == admittedAt && WIFEXITED(strangerStatus) && WEXITSTATUS(strangerStatus) != 0 &&
            WEXITSTATUS(strangerStatus) < 126, "unrelated process delivered on another inbox socket");

    // A large bus message goes to the attachment receiver; the narrower driver JSON-line limit does not apply.
    const std::string largeBody(1024 * 1024 + 1, 'a');
    writeFile(home.root / "replacement.wire", "1730800000001-1\tsender\t-\tanother\t" + largeBody + "\n");
    listeners.configure({{"replacement", "127.0.0.1:6379"}});
    bool offered = false;
    const auto replacement = [&](const std::string& inbox, const BusMessage& message) {
        require(inbox == "replacement" && message.replyTo.empty(), "wrong recipient or absent reply reference");
        require(message.body == largeBody, "large bus message body changed in transport");
        offered = true;
        return BusAccept::Admit;
    };
    until(listeners, [&] { return offered; }, replacement, "replacement listener received nothing");
    int pid = -1;
    std::ifstream(home.root / "replacement.pid") >> pid;
    require(pid > 0, "missing replacement hook PID");
    listeners.configure({});
    listeners.flushed();
    until(listeners, [&] { return ::kill(pid, 0) < 0 && errno == ESRCH; }, replacement, "removed recipient still holds a bus hook");
    require(!fs::exists(home.root / "replacement.acked"), "revoked recipient acknowledged an unflushed message");

    // The real hook stays alive and retries an unavailable Redis. Its free-form stderr can carry sensitive data.
    const auto beforeOutage = reports.size();
    listeners.configure({{"offline", "127.0.0.1:6379"}});
    until(listeners, [&] { return reports.size() > beforeOutage && fs::exists(home.root / "offline.reported"); },
          replacement, "Redis outage was silent");
    listeners.serve(replacement); // both diagnostics are now in the hook's pipe or already consumed
    std::ifstream(home.root / "offline.pid") >> pid;
    require(pid > 0 && ::kill(pid, 0) == 0, "fake hook ended instead of waiting for Redis");
    require(reports.size() == beforeOutage + 1 && reports.back() ==
            "message bus for offline: the bus is unavailable; check its Redis endpoint and credentials",
            "repeated outages did not produce one safe diagnostic");
    listeners.configure({});
    until(listeners, [&] { return ::kill(pid, 0) < 0 && errno == ESRCH; }, replacement, "offline hook did not exit");
    require(fs::exists(home.root / "offline.term"), "hook never received SIGTERM to release its Redis presence");
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "bus-deliver") return nexus::server::busDeliver(argv[2]);
    try {
        exercise(fs::canonical(argv[0]));
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bus listener functional: %s\n", ex.what());
        return 1;
    }
}
