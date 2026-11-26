// nexus-link-relay: the window's link, run inside WSL2 for the native window on Windows (relay/relay_wire.h says why).
//
//   nexus-link-relay (--local | --server HOST) [-- COMMAND...]
//
// It keeps the link a window on macOS or Linux keeps (link/client_port.h: the bootstrap, mosh's session, the redial and
// the history pages) and speaks the relay protocol on its standard input and output: the window's actions and history
// requests in, the link's state out. --local runs the bootstrap on this machine without ssh (the server in this WSL2
// distribution); --server HOST runs it over ssh from here, as a window on Linux would. COMMAND replaces `nexus-server
// new` (a test's server with a home of its own). Its log lines go to standard error, which the window writes to its log.
// It ends when its standard input ends (the window quit or closed the pipe), after mosh's shutdown, with 0; when the
// window's bytes are not the relay protocol, with 1 and why; and on bad arguments with 2.

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "link/client_port.h"
#include "relay/relay_ends.h"

namespace {

using namespace nexus;

// Writes all of `bytes` to standard output; false when the window's end is gone.
bool writeAll(const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(STDOUT_FILENO, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

int usage() {
    std::fprintf(stderr, "usage: nexus-link-relay (--local | --server HOST) [-- COMMAND...]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    link::ClientLink::Options options;
    bool chosen = false;
    int i = 1;
    for (; i < argc; ++i) {
        if (std::strcmp(argv[i], "--local") == 0 && !chosen) {
            options.bootstrap.local = true;
            options.bootstrap.host = "localhost";
            options.ip = "127.0.0.1";
            chosen = true;
        } else if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc && !chosen) {
            options.bootstrap.host = argv[++i];
            chosen = true;
        } else if (std::strcmp(argv[i], "--") == 0 && i + 1 < argc) {
            options.bootstrap.command.assign(argv + i + 1, argv + argc);
            break;
        } else {
            return usage();
        }
    }
    if (!chosen) return usage();
    // A write to a window that has gone is an error to read, never a signal that ends the relay mid-shutdown.
    ::signal(SIGPIPE, SIG_IGN);

    std::mutex mutex;
    std::condition_variable woken;
    bool wake = false;
    bool inputEnded = false;
    std::string refusal;
    relay::RelayServer ends;

    options.wake = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        wake = true;
        woken.notify_one();
    };
    if (!writeAll(ends.open())) return 0;
    auto port = std::make_unique<link::ClientPort>(options);

    // Standard input on a thread of its own: the window's frames, handed to the loop below.
    std::thread reader([&] {
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            std::lock_guard<std::mutex> lock(mutex);
            if (n <= 0) {
                inputEnded = true;
            } else {
                std::string why;
                if (!ends.feed(buf, static_cast<std::size_t>(n), &why)) {
                    refusal = why;
                    inputEnded = true;
                }
            }
            volatile char* p = buf;
            for (ssize_t k = 0; k < n; ++k) p[k] = 0;
            wake = true;
            woken.notify_one();
            if (inputEnded) return;
        }
    });

    bool windowGone = false;
    for (;;) {
        std::vector<relay::RelayServer::Request> requests;
        bool ending = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            woken.wait_for(lock, std::chrono::milliseconds(250), [&] { return wake; });
            wake = false;
            requests = ends.take();
            ending = inputEnded;
        }
        for (auto& r : requests) {
            if (r.kind == relay::RelayServer::Request::Kind::Action) {
                port->send(std::move(r.action));
            } else {
                port->requestHistory(r.session, r.epoch, r.before);
            }
        }
        LinkState state;
        if (port->poll(&state) && !writeAll(ends.state(state))) {
            windowGone = true;
        }
        if (ending || windowGone) break;
    }
    port->close();      // mosh's shutdown, so the server ends this session (up to two seconds)
    bool readerDone;
    {
        std::lock_guard<std::mutex> lock(mutex);
        readerDone = inputEnded;
    }
    // A reader still blocked on a window that closed only its read end goes with the process.
    if (readerDone) reader.join();
    else reader.detach();
    if (!refusal.empty()) {
        std::fprintf(stderr, "nexus-link-relay: %s\n", refusal.c_str());
        return 1;
    }
    return 0;
}
