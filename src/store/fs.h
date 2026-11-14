#pragma once
#include "store/codec.h"
#include "store/status.h"

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nexus::store {

// The filesystem the store writes through: the seam that lets the unit tests run on MemFs, in memory, and crash it
// at any write or fsync. Paths are "dir/name"; every rename stays inside one directory.
//
// What the store relies on, and what MemFs reproduces:
//   - bytes written to a file are durable once sync(file) returns; before that a crash may keep any prefix of them,
//     with any run of the kept bytes damaged (a torn write), or lose them all;
//   - a create, rename or remove is durable once sync_dir(dir) returns; before that a crash may keep any subset of
//     the directory's pending changes, applied in order; a rename is atomic (both names change or neither);
//   - make_dir is durable when it returns.
//
// A file the store appends to all the time (the journal's log) is opened once, by open_append, and kept open: an
// AppendFile is that file, not its name, as an open descriptor is. Once another file is renamed over its name, or the
// filesystem crashes, the AppendFile still refers to what it opened, so its owner opens the name again.
class AppendFile {
public:
    virtual ~AppendFile() = default;
    virtual Status append(std::span<const std::uint8_t> data) = 0;
    virtual Status sync() = 0;
};

class Fs {
public:
    virtual ~Fs() = default;
    virtual Status open_append(const std::string& path, std::unique_ptr<AppendFile>& out) = 0;  // NotFound if missing
    virtual Status make_dir(const std::string& dir) = 0;   // with its parents; nothing if it is there
    virtual Status list(const std::string& dir, std::vector<std::string>& names) = 0;  // NotFound if missing
    virtual Status size(const std::string& path, std::uint64_t& out) = 0;               // NotFound if missing
    // Up to `n` bytes from `off` (fewer at the end of the file). NotFound if missing.
    virtual Status read(const std::string& path, std::uint64_t off, std::uint64_t n, Bytes& out) = 0;
    virtual Status create(const std::string& path) = 0;    // a new empty file; Failed if the name is taken
    virtual Status append(const std::string& path, std::span<const std::uint8_t> data) = 0;
    virtual Status truncate(const std::string& path, std::uint64_t size) = 0;
    virtual Status sync(const std::string& path) = 0;
    virtual Status sync_dir(const std::string& dir) = 0;
    virtual Status rename(const std::string& from, const std::string& to) = 0;  // replaces `to`
    virtual Status remove(const std::string& path) = 0;

    Status read_all(const std::string& path, Bytes& out) {
        return read(path, 0, std::numeric_limits<std::uint64_t>::max(), out);
    }
};

// The real filesystem (POSIX). sync is F_FULLFSYNC on macOS, which flushes the drive's cache too (plain fsync there
// does not survive a power cut); fsync elsewhere.
std::unique_ptr<Fs> make_posix_fs();

std::string dir_of(const std::string& path);
std::string name_of(const std::string& path);

// A deterministic pseudo-random source for crash choices and walks: splitmix64, from a seed alone.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next();
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }

private:
    std::uint64_t s_;
};

// The filesystem in memory, for the unit tests and the trace walk. Each file holds the bytes a reader sees now and
// the bytes that survive a crash; each directory holds its names now, its names that survive, and the changes in
// between, in order.
//
// Crash injection: fail_at(k) makes the k-th mutating call from now (create, append, truncate, sync, sync_dir,
// rename, remove, and an AppendFile's append and sync; 1 is the next one) fail without effect, as if the process died
// just before it, and every later call fail too. crash() then turns what is volatile into one state a real crash
// could leave, and the store is reopened on it.
//
// An AppendFile is bound to the file it opened (its inode), as a descriptor is: after a rename over its name it writes
// to the file that is no longer named, whose bytes no crash keeps; after crash() every AppendFile opened before it
// refuses, as the process that held it is gone. A store that does not open its log again after a compaction or a
// crash loses records in the unit tests as it would on disk.
class MemFs : public Fs {
public:
    // How one file came through a crash. `revert`: the durable bytes and nothing written since. Otherwise the bytes
    // the file shares with its durable ones, then `keep` more of those written since, with [bad_from, bad_to)
    // (absolute offsets, empty when equal) damaged.
    struct FileFate {
        bool revert = false;
        std::uint64_t keep = 0;
        std::uint64_t bad_from = 0;
        std::uint64_t bad_to = 0;
    };
    // One crash: per directory, which of its pending changes (by index) survive; per surviving file (by path), its
    // fate. A file or directory not named keeps its durable state.
    struct CrashPlan {
        std::map<std::string, std::vector<bool>> dir_keep;
        std::map<std::string, FileFate> files;
    };
    struct DirOp {
        std::string from;  // removed or renamed name ("" for a create)
        std::string to;    // created or renamed-to name ("" for a remove)
        int ino = -1;
    };

    // A copy is the disk as it is now, for the tests to crash in different ways: the AppendFiles open on this one
    // stay with it, and none is open on the copy.
    MemFs() = default;
    MemFs(const MemFs& other);
    MemFs& operator=(const MemFs&) = delete;

    Status open_append(const std::string& path, std::unique_ptr<AppendFile>& out) override;
    Status make_dir(const std::string& dir) override;
    Status list(const std::string& dir, std::vector<std::string>& names) override;
    Status size(const std::string& path, std::uint64_t& out) override;
    Status read(const std::string& path, std::uint64_t off, std::uint64_t n, Bytes& out) override;
    Status create(const std::string& path) override;
    Status append(const std::string& path, std::span<const std::uint8_t> data) override;
    Status truncate(const std::string& path, std::uint64_t size) override;
    Status sync(const std::string& path) override;
    Status sync_dir(const std::string& dir) override;
    Status rename(const std::string& from, const std::string& to) override;
    Status remove(const std::string& path) override;

    void fail_at(std::uint64_t k);       // 0: never
    bool crashed() const { return dead_; }
    std::uint64_t calls() const { return calls_; }  // mutating calls made (or failed) so far

    // A plan drawn from `rng`: each pending directory change kept or not, each changed file reverted or torn.
    CrashPlan plan(Rng& rng) const;
    // A plan that keeps everything volatile (the machine did not crash; the process did) or nothing (`all`
    // false): what a kill -9 leaves, and the harshest crash.
    CrashPlan plan_all(bool all) const;
    void crash(const CrashPlan& plan);   // applies the plan; the filesystem is live again

    // For the trace walk and the tests: the bytes now and the durable bytes of a file; the names of a directory.
    bool has(const std::string& path) const;
    const Bytes& data(const std::string& path) const;
    const Bytes& durable(const std::string& path) const;
    const std::vector<DirOp>& pending(const std::string& dir) const;
    std::map<std::string, std::uint64_t> sizes(const std::string& dir) const;  // each name's size now
    // Damages one byte of a file's durable copy and its live copy (a disk fault the store must report, not hide).
    void corrupt(const std::string& path, std::uint64_t off);

private:
    struct File {
        Bytes data;
        Bytes dur;
    };
    struct Dir {
        std::map<std::string, int> now;
        std::map<std::string, int> dur;
        std::vector<DirOp> pending;
    };
    class Open;
    Status step(const char* what, const std::string& path);
    File* file(const std::string& path);
    const File* file(const std::string& path) const;
    int new_file();
    void collect();

    std::map<std::string, Dir> dirs_;
    std::vector<File> files_;
    std::vector<bool> live_;
    std::map<int, int> opened_;     // inode: AppendFiles open on it (kept alive while any is)
    std::uint64_t epoch_ = 0;       // crashes so far: an AppendFile from an earlier epoch refuses
    std::uint64_t calls_ = 0;
    std::uint64_t fail_at_ = 0;
    bool dead_ = false;
};

} // namespace nexus::store
