// Functional test of the server's durable state on the real filesystem, in the test's own working directory (ctest
// sets it to build/tests/store/functional): open, change, reopen; a blob uploaded and placed; then a child process
// that commits as fast as it can is killed with SIGKILL, and every change it had acknowledged must be there when the
// store is opened again.

#include "store/codec.h"
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

const std::uint64_t kNow = 2000000000;

std::span<const std::uint8_t> span_of(const std::string& s) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void basic(Fs& fs) {
    std::unique_ptr<Store> s;
    OpenReport rep;
    CHECK_OK(Store::open(fs, "basic", kNow, {}, s, &rep));
    if (!s)
        return;
    CHECK(rep.fresh);
    CHECK_OK(s->send(1, "hello"));
    CHECK_OK(s->open_entry(1, "wor"));
    CHECK_OK(s->grow(1, "ld"));
    CHECK_OK(s->enqueue(1, "queued"));
    std::string content(100000, 'z');
    Hash h = sha256(span_of(content));
    BlobState b;
    CHECK_OK(s->offer(h, content.size(), b));
    CHECK_OK(s->chunk(h, 0, 0, span_of(content.substr(0, 40000)), b));
    s.reset();
    CHECK_OK(Store::open(fs, "basic", kNow, {}, s, &rep));
    if (!s)
        return;
    CHECK(!rep.fresh && !rep.restored && s->incarnation() == kNow + 1);
    Snap sn = s->snapshot(1, 10);
    CHECK(sn.version == 4 && sn.entries.size() == 2 && sn.entries[1].text == "world" && sn.queue.size() == 1);
    b = s->blob(h);
    CHECK(b.held == 40000);
    CHECK_OK(s->chunk(h, 0, 40000, span_of(content.substr(40000)), b));
    CHECK(b.complete);
    ClientId c{};
    c[0] = 1;
    ActResult r;
    std::vector<Action> acts = {{1, 2, Action::Kind::Send, "attached", h, "big file.bin"}};
    CHECK_OK(s->apply(c, acts, r));
    CHECK(r.applied == 1);
    CHECK(std::filesystem::file_size("basic/incoming/2/big_file.bin") == content.size());
    CHECK(!std::filesystem::exists("basic/incoming/2/.nexus-big_file.bin.tmp"));
}

// The child: opens the store and sends "m<i>" to tab 1 for i = 1, 2, ... forever, writing i to the pipe after each
// send returns Ok (the acknowledgement).
[[noreturn]] void child(int fd) {
    std::unique_ptr<Fs> fs = make_posix_fs();
    std::unique_ptr<Store> s;
    StoreOptions options;
    options.compact_bytes = 2000;  // compactions happen while it runs, so a kill can land in one
    if (!Store::open(*fs, "killed", kNow, options, s).is_ok())
        _exit(3);
    for (std::uint64_t i = 1;; ++i) {
        if (!s->send(1, "m" + std::to_string(i)).is_ok())
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
        // past 30 + 20 * round acknowledgements, kill it wherever it is
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
        CHECK_OK(Store::open(*fs, "killed", kNow, options, s, &rep));
        if (!s)
            return;
        CHECK(!rep.restored);
        Snap sn = s->snapshot(1, 100000);
        // every acknowledged send is there, in order, from the start; nothing else is
        CHECK(sn.entries.size() >= acked);
        for (std::size_t i = 0; i < sn.entries.size(); ++i)
            CHECK(sn.entries[i].text == "m" + std::to_string(i + 1));
        std::printf("round %d: %llu acknowledged, %zu recovered, %llu bytes of torn tail cut\n", round,
                    static_cast<unsigned long long>(acked), sn.entries.size(),
                    static_cast<unsigned long long>(rep.store_cut));
        // the next round's child appends after these; start the numbering again from a fresh store
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
