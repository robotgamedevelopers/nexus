// Functional test of the server's durable state on the real filesystem, in the test's own working directory (ctest
// sets it to build/tests/store/functional): open, commit, reopen; then a child process that commits as fast as it can
// is killed with SIGKILL, and every step it had committed must be there when the store is opened again (at once: the
// dead child's lock is gone with it); then the store's lock itself (locks()).

#include "store/fs.h"
#include "store/store.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
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

// The step that makes session 1, with ada.
std::vector<Change> made() {
    Change c;
    c.kind = Change::Kind::SetInfo;
    c.info = Info{"ada", "claude", "", "", ""};
    return {c};
}

void basic(Fs& fs) {
    std::unique_ptr<Store> s;
    OpenReport rep;
    CHECK_OK(Store::open(fs, "basic", {}, s, &rep));
    if (!s)
        return;
    CHECK(rep.fresh);
    CHECK_OK(s->commit(1, made()));
    CHECK_OK(s->commit(1, prompt("hello"), "w", 1));
    Change q;
    q.kind = Change::Kind::Enqueue;
    q.prompt = "queued";
    CHECK_OK(s->commit(1, std::vector<Change>{q}, "w", 2));
    s.reset();
    CHECK_OK(Store::open(fs, "basic", {}, s, &rep));
    if (!s)
        return;
    CHECK(!rep.fresh && rep.records == 3);
    CHECK(s->applied("w") == 2 && s->next_session() == 2);
    const Session* t = s->session(1);
    CHECK(t != nullptr && t->transcript.size() == 1 && t->transcript[0].text == "hello" && t->queue.size() == 1);
}

// The child: opens the store, makes session 1, and commits "m<i>" to it as window w's action i, for i = 1, 2, ...
// forever, writing i to the pipe after each commit returns Ok (what the server would then acknowledge).
[[noreturn]] void child(int fd) {
    std::unique_ptr<Fs> fs = make_posix_fs();
    std::unique_ptr<Store> s;
    StoreOptions options;
    options.compact_bytes = 2000;  // compactions happen while it runs, so a kill can land in one
    if (!Store::open(*fs, "killed", options, s).is_ok())
        _exit(3);
    if (!s->commit(1, made()).is_ok()) {
        _exit(4);
    }
    for (std::uint64_t i = 1;; ++i) {
        if (!s->commit(1, prompt("m" + std::to_string(i)), "w", i).is_ok()) {
            _exit(4);
        }
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
        const Session* t = s->session(1);
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

// The store's lock on the real filesystem (flock on the store's directory, PosixFs::lock): no lock file is made; a
// second writer is Busy at once, in this process or another, and by any name the directory is reached by; a reader is
// refused while a writer holds it and changes and makes nothing; a writer waits for readers only, and past the wait
// fails (never Busy, which is another writer); the holder of a directory moved aside goes on writing in it, never in a
// new directory made at its old name; the refusals where locks cannot be trusted; and a holder killed with SIGKILL
// leaves no lock behind (that is kill_and_reopen, which opens again at once).
void locks() {
    std::unique_ptr<Fs> fs = make_posix_fs();
    std::unique_ptr<Store> a;
    CHECK_OK(Store::open(*fs, "locked", {}, a));
    if (!a) {
        return;
    }
    CHECK_OK(a->commit(1, made()));
    // The lock is the directory's: nothing is made for it.
    CHECK(!std::filesystem::exists("locked/lock"));
    // A second open, on a descriptor of its own: Busy, whether in this process or a child.
    std::unique_ptr<Fs> fs2 = make_posix_fs();
    std::unique_ptr<Store> b;
    Status busy = Store::open(*fs2, "locked", {}, b);
    CHECK(busy.code() == Status::Code::Busy && b == nullptr);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        std::unique_ptr<Fs> cfs = make_posix_fs();
        std::unique_ptr<Store> c;
        _exit(Store::open(*cfs, "locked", {}, c).code() == Status::Code::Busy ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    // The same directory by another name (a symbolic link to it from elsewhere, as a second server directory whose
    // store is a link would reach it): the one lock, Busy.
    std::filesystem::remove_all("elsewhere");
    std::filesystem::create_directory("elsewhere");
    std::filesystem::create_directory_symlink(std::filesystem::absolute("locked"), "elsewhere/store");
    CHECK(Store::open(*fs2, "elsewhere/store", {}, b).code() == Status::Code::Busy && b == nullptr);
    // A reader while the writer has it: refused, nothing read, nothing made.
    MemFs copy;
    std::unique_ptr<Store> r;
    CHECK(Store::inspect(*fs2, "locked", copy, r).code() == Status::Code::Busy && r == nullptr);
    std::uintmax_t size = std::filesystem::file_size("locked/log");
    a.reset();
    // Without it: read, and the files are as they were, and none made.
    MemFs copy2;
    CHECK_OK(Store::inspect(*fs2, "locked", copy2, r));
    CHECK(r != nullptr && r->sessions().size() == 1);
    CHECK(std::filesystem::file_size("locked/log") == size);
    CHECK(!std::filesystem::exists("locked/lock"));
    r.reset();

    // A writer waits for a reader that lets go within kLockReaderWaitMs, and has the store then. The reader is a child
    // (a fork shares its parent's descriptors, so it holds nothing of this process's).
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        std::unique_ptr<Fs> cfs = make_posix_fs();
        std::unique_ptr<FileLock> held;
        if (!cfs->lock("locked", LockMode::Shared, held).is_ok()) {
            _exit(1);
        }
        char go = 1;
        if (write(fds[1], &go, 1) != 1) {
            _exit(2);
        }
        usleep(200 * 1000);
        _exit(0);
    }
    close(fds[1]);
    char go = 0;
    CHECK(read(fds[0], &go, 1) == 1);
    close(fds[0]);
    const auto opened = std::chrono::steady_clock::now();
    CHECK_OK(Store::open(*fs, "locked", {}, b));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened);
    CHECK(b != nullptr && took.count() < static_cast<long long>(kLockReaderWaitMs));
    std::printf("locks: a writer waited %lld ms for a reader to let go\n", static_cast<long long>(took.count()));
    b.reset();
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // One that holds on past kLockReaderWaitMs: the writer gives up, Failed (a start that failed, not another server),
    // saying that readers held it.
    std::unique_ptr<FileLock> reader;
    CHECK_OK(fs2->lock("locked", LockMode::Shared, reader));
    const auto start = std::chrono::steady_clock::now();
    busy = Store::open(*fs, "locked", {}, b);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    CHECK(busy.code() == Status::Code::Failed && b == nullptr);
    CHECK(busy.message().find("held by readers only") != std::string::npos);
    CHECK(waited.count() >= static_cast<long long>(kLockReaderWaitMs));
    std::printf("locks: a writer gave up on a reader after %lld ms: %s\n", static_cast<long long>(waited.count()),
                busy.message().c_str());
    reader.reset();

    // The directory moved aside while its writer holds it, and a new store made at its old name: the writer's records
    // and its compaction's renames go on in the moved directory, through its hold, and the new store is untouched.
    {
        std::filesystem::remove_all("moved");
        std::unique_ptr<Fs> wfs = make_posix_fs();
        std::unique_ptr<Store> w;
        StoreOptions small;
        small.compact_bytes = 300;
        CHECK_OK(Store::open(*wfs, "locked", small, w));
        std::filesystem::rename("locked", "moved");
        std::unique_ptr<Fs> nfs = make_posix_fs();
        std::unique_ptr<Store> fresh;
        CHECK_OK(Store::open(*nfs, "locked", {}, fresh));     // its own directory, its own lock
        CHECK_OK(fresh->commit(1, made()));
        const std::uintmax_t newSize = std::filesystem::file_size("locked/log");
        for (std::uint64_t i = 1; i <= 20; ++i) {
            CHECK_OK(w->commit(1, prompt("old " + std::to_string(i))));
        }
        CHECK(std::filesystem::exists("moved/snapshot"));      // it compacted, in the moved directory
        CHECK(!std::filesystem::exists("locked/snapshot"));
        CHECK(std::filesystem::file_size("locked/log") == newSize);
        w.reset();
        fresh.reset();
        std::unique_ptr<Store> back;
        CHECK_OK(Store::open(*wfs, "moved", {}, back));
        CHECK(back != nullptr && back->session(1) != nullptr && back->session(1)->transcript.size() == 20);
        back.reset();
        std::unique_ptr<Store> other;
        CHECK_OK(Store::open(*nfs, "locked", {}, other));
        CHECK(other != nullptr && other->session(1) != nullptr && other->session(1)->transcript.empty());
        other.reset();
        std::filesystem::remove_all("moved");
    }

    // Where locks cannot be trusted, nothing runs unlocked: a network filesystem (the seam reports one), and one where
    // a second exclusive lock is granted beside the first (the seam grants it).
    {
        PosixOptions nfsLike;
        nfsLike.test_fs_type = "nfs";
        std::unique_ptr<Fs> net = make_posix_fs(nfsLike);
        std::unique_ptr<Store> s;
        const Status refused = Store::open(*net, "locked", {}, s);
        CHECK(refused.code() == Status::Code::Failed && s == nullptr);
        CHECK(refused.message().find("is on NFS, a network filesystem") != std::string::npos);
        std::printf("locks: %s\n", refused.message().c_str());
        MemFs c;
        CHECK(Store::inspect(*net, "locked", c, s).code() == Status::Code::Failed);
        PosixOptions ignored;
        ignored.test_locks_ignored = true;
        std::unique_ptr<Fs> lax = make_posix_fs(ignored);
        const Status unkept = Store::open(*lax, "locked", {}, s);
        CHECK(unkept.code() == Status::Code::Failed && s == nullptr);
        CHECK(unkept.message().find("are not kept") != std::string::npos);
        std::printf("locks: %s\n", unkept.message().c_str());
        // Neither refusal left a lock behind.
        CHECK_OK(Store::open(*fs, "locked", {}, s));
    }
    std::filesystem::remove_all("elsewhere");
    std::filesystem::remove_all("locked");
}

} // namespace

int main() {
    std::filesystem::remove_all("basic");
    std::filesystem::remove_all("killed");
    std::filesystem::remove_all("locked");
    std::unique_ptr<Fs> fs = make_posix_fs();
    basic(*fs);
    kill_and_reopen();
    locks();
    if (failures != 0) {
        std::fprintf(stderr, "store_functional_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("store_functional_test: ok\n");
    return 0;
}
