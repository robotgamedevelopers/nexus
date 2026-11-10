// Functional test of the server's durable state on the real filesystem, in the test's own working directory (ctest
// sets it to build/tests/store/functional): open, commit, reopen; then a child process that commits as fast as it can
// is killed with SIGKILL, and every step it had committed must be there when the store is opened again.

#include "store/fs.h"
#include "store/store.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace nexus::store;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_OK(expr)                                                                                       \
    do {                                                                                                     \
        Status s_ = (expr);                                                                                  \
        if (!s_.is_ok()) {                                                                                   \
            std::fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__, __LINE__, #expr, s_.message().c_str()); \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (0)

std::vector<Change> prompt(const std::string& text) {
    Change c;
    c.entry = Entry{0, text, true};
    return {c};
}

void basic(Fs& fs) {
    std::unique_ptr<Store> s;
    OpenReport rep;
    CHECK_OK(Store::open(fs, "basic", {}, s, &rep));
    if (!s)
        return;
    CHECK(rep.fresh);
    CHECK_OK(s->commit("ada", prompt("hello"), "w", 1));
    Change q;
    q.kind = Change::Kind::Enqueue;
    q.prompt = "queued";
    CHECK_OK(s->commit("ada", std::vector<Change>{q}, "w", 2));
    s.reset();
    CHECK_OK(Store::open(fs, "basic", {}, s, &rep));
    if (!s)
        return;
    CHECK(!rep.fresh && rep.records == 2);
    CHECK(s->applied("w") == 2);
    const Tab* t = s->tab("ada");
    CHECK(t != nullptr && t->transcript.size() == 1 && t->transcript[0].text == "hello" && t->queue.size() == 1);
}

// The child: opens the store and commits "m<i>" to tab ada as window w's action i, for i = 1, 2, ... forever, writing
// i to the pipe after each commit returns Ok (what the server would then acknowledge).
[[noreturn]] void child(int fd) {
    std::unique_ptr<Fs> fs = make_posix_fs();
    std::unique_ptr<Store> s;
    StoreOptions options;
    options.compact_bytes = 2000;  // compactions happen while it runs, so a kill can land in one
    if (!Store::open(*fs, "killed", options, s).is_ok())
        _exit(3);
    for (std::uint64_t i = 1;; ++i) {
        if (!s->commit("ada", prompt("m" + std::to_string(i)), "w", i).is_ok())
            _exit(4);
        if (write(fd, &i, sizeof i) != sizeof i)
            _exit(5);
    }
}

void kill_and_reopen() {
    for (int round = 0; round < 3; ++round) {
        int fds[2];
        CHECK(pipe(fds) == 0);
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            close(fds[0]);
            child(fds[1]);
        }
        close(fds[1]);
        std::uint64_t acked = 0;
        std::uint64_t got = 0;
        // past 30 + 20 * round commits, kill it wherever it is
        while (acked < 30 + 20 * std::uint64_t(round) && read(fds[0], &got, sizeof got) == sizeof got)
            acked = got;
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        close(fds[0]);
        CHECK(WIFSIGNALED(status));

        std::unique_ptr<Fs> fs = make_posix_fs();
        std::unique_ptr<Store> s;
        StoreOptions options;
        options.compact_bytes = 2000;
        OpenReport rep;
        CHECK_OK(Store::open(*fs, "killed", options, s, &rep));
        if (!s)
            return;
        // every committed step is there, in order, from the start, and the counter agrees with the transcript
        const Tab* t = s->tab("ada");
        std::size_t n = t == nullptr ? 0 : t->transcript.size();
        CHECK(n >= acked);
        CHECK(s->applied("w") == n);
        for (std::size_t i = 0; i < n; ++i)
            CHECK(t->transcript[i].text == "m" + std::to_string(i + 1));
        std::printf("round %d: %llu committed, %zu recovered, %llu bytes of torn tail cut\n", round,
                    static_cast<unsigned long long>(acked), n, static_cast<unsigned long long>(rep.cut_bytes));
        s.reset();
        std::filesystem::remove_all("killed");
    }
}

} // namespace

int main() {
    std::filesystem::remove_all("basic");
    std::filesystem::remove_all("killed");
    std::unique_ptr<Fs> fs = make_posix_fs();
    basic(*fs);
    kill_and_reopen();
    if (failures != 0) {
        std::fprintf(stderr, "store_functional_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("store_functional_test: ok\n");
    return 0;
}
