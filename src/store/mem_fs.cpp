// MemFs: the filesystem in memory, with the crash semantics fs.h states. Used by the unit tests and the trace walk.

#include "store/fs.h"

#include <algorithm>

namespace nexus::store {

std::string dir_of(const std::string& path) {
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string name_of(const std::string& path) {
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::uint64_t Rng::next() {
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

namespace {

std::uint64_t common_prefix(const Bytes& a, const Bytes& b) {
    std::uint64_t n = std::min(a.size(), b.size());
    std::uint64_t i = 0;
    while (i < n && a[i] == b[i])
        ++i;
    return i;
}

std::map<std::string, int> apply_kept(const std::map<std::string, int>& dur, const std::vector<MemFs::DirOp>& ops,
                                      const std::vector<bool>* keep) {
    std::map<std::string, int> out = dur;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (keep == nullptr || i >= keep->size() || !(*keep)[i])
            continue;
        if (!ops[i].from.empty())
            out.erase(ops[i].from);
        if (!ops[i].to.empty())
            out[ops[i].to] = ops[i].ino;
    }
    return out;
}

} // namespace

Status MemFs::step(const char* what, const std::string& path) {
    if (dead_)
        return Status::failed(std::string("memfs: ") + what + " " + path + " after an injected crash");
    ++calls_;
    if (fail_at_ != 0 && calls_ == fail_at_) {
        dead_ = true;
        return Status::failed(std::string("memfs: injected crash before ") + what + " " + path);
    }
    return Status::ok();
}

MemFs::File* MemFs::file(const std::string& path) {
    auto d = dirs_.find(dir_of(path));
    if (d == dirs_.end())
        return nullptr;
    auto n = d->second.now.find(name_of(path));
    return n == d->second.now.end() ? nullptr : &files_[std::size_t(n->second)];
}

const MemFs::File* MemFs::file(const std::string& path) const {
    auto d = dirs_.find(dir_of(path));
    if (d == dirs_.end())
        return nullptr;
    auto n = d->second.now.find(name_of(path));
    return n == d->second.now.end() ? nullptr : &files_[std::size_t(n->second)];
}

int MemFs::new_file() {
    for (std::size_t i = 0; i < live_.size(); ++i) {
        if (!live_[i]) {
            live_[i] = true;
            files_[i] = File{};
            return int(i);
        }
    }
    files_.push_back(File{});
    live_.push_back(true);
    return int(files_.size() - 1);
}

void MemFs::collect() {
    std::vector<bool> used(files_.size(), false);
    for (const auto& [path, d] : dirs_) {
        for (const auto& [n, ino] : d.now)
            used[std::size_t(ino)] = true;
        for (const auto& [n, ino] : d.dur)
            used[std::size_t(ino)] = true;
        for (const DirOp& op : d.pending) {
            if (op.ino >= 0)
                used[std::size_t(op.ino)] = true;
        }
    }
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (!used[i] && live_[i]) {
            live_[i] = false;
            files_[i] = File{};
        }
    }
}

Status MemFs::make_dir(const std::string& dir) {
    if (dead_)
        return Status::failed("memfs: make_dir " + dir + " after an injected crash");
    std::string at;
    std::size_t start = 0;
    while (start <= dir.size()) {
        std::size_t slash = dir.find('/', start);
        std::size_t end = slash == std::string::npos ? dir.size() : slash;
        at = dir.substr(0, end);
        if (!at.empty())
            dirs_.try_emplace(at);
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return Status::ok();
}

Status MemFs::list(const std::string& dir, std::vector<std::string>& names) {
    names.clear();
    auto d = dirs_.find(dir);
    if (d == dirs_.end())
        return Status::not_found("memfs: no directory " + dir);
    for (const auto& [n, ino] : d->second.now)
        names.push_back(n);
    return Status::ok();
}

Status MemFs::size(const std::string& path, std::uint64_t& out) {
    const File* f = file(path);
    if (f == nullptr)
        return Status::not_found("memfs: no file " + path);
    out = f->data.size();
    return Status::ok();
}

Status MemFs::read(const std::string& path, std::uint64_t off, std::uint64_t n, Bytes& out) {
    out.clear();
    const File* f = file(path);
    if (f == nullptr)
        return Status::not_found("memfs: no file " + path);
    if (off >= f->data.size())
        return Status::ok();
    std::uint64_t end = off + std::min<std::uint64_t>(n, f->data.size() - off);
    out.assign(f->data.begin() + std::ptrdiff_t(off), f->data.begin() + std::ptrdiff_t(end));
    return Status::ok();
}

Status MemFs::create(const std::string& path) {
    Status s = step("create", path);
    if (!s.is_ok())
        return s;
    auto d = dirs_.find(dir_of(path));
    if (d == dirs_.end())
        return Status::not_found("memfs: no directory " + dir_of(path));
    if (d->second.now.count(name_of(path)) != 0)
        return Status::failed("memfs: create " + path + ": the name is taken");
    int ino = new_file();
    d->second.now[name_of(path)] = ino;
    d->second.pending.push_back(DirOp{"", name_of(path), ino});
    return Status::ok();
}

Status MemFs::append(const std::string& path, std::span<const std::uint8_t> data) {
    Status s = step("append", path);
    if (!s.is_ok())
        return s;
    File* f = file(path);
    if (f == nullptr)
        return Status::not_found("memfs: no file " + path);
    f->data.insert(f->data.end(), data.begin(), data.end());
    return Status::ok();
}

Status MemFs::truncate(const std::string& path, std::uint64_t size) {
    Status s = step("truncate", path);
    if (!s.is_ok())
        return s;
    File* f = file(path);
    if (f == nullptr)
        return Status::not_found("memfs: no file " + path);
    f->data.resize(size, 0);
    return Status::ok();
}

Status MemFs::sync(const std::string& path) {
    Status s = step("sync", path);
    if (!s.is_ok())
        return s;
    File* f = file(path);
    if (f == nullptr)
        return Status::not_found("memfs: no file " + path);
    f->dur = f->data;
    return Status::ok();
}

Status MemFs::sync_dir(const std::string& dir) {
    Status s = step("sync_dir", dir);
    if (!s.is_ok())
        return s;
    auto d = dirs_.find(dir);
    if (d == dirs_.end())
        return Status::not_found("memfs: no directory " + dir);
    d->second.dur = d->second.now;
    d->second.pending.clear();
    collect();
    return Status::ok();
}

Status MemFs::rename(const std::string& from, const std::string& to) {
    Status s = step("rename", from);
    if (!s.is_ok())
        return s;
    if (dir_of(from) != dir_of(to))
        return Status::failed("memfs: rename across directories: " + from + " to " + to);
    auto d = dirs_.find(dir_of(from));
    if (d == dirs_.end() || d->second.now.count(name_of(from)) == 0)
        return Status::not_found("memfs: no file " + from);
    int ino = d->second.now[name_of(from)];
    d->second.now.erase(name_of(from));
    d->second.now[name_of(to)] = ino;
    d->second.pending.push_back(DirOp{name_of(from), name_of(to), ino});
    return Status::ok();
}

Status MemFs::remove(const std::string& path) {
    Status s = step("remove", path);
    if (!s.is_ok())
        return s;
    auto d = dirs_.find(dir_of(path));
    if (d == dirs_.end() || d->second.now.count(name_of(path)) == 0)
        return Status::not_found("memfs: no file " + path);
    d->second.now.erase(name_of(path));
    d->second.pending.push_back(DirOp{name_of(path), "", -1});
    return Status::ok();
}

void MemFs::fail_at(std::uint64_t k) {
    fail_at_ = k == 0 ? 0 : calls_ + k;
}

MemFs::CrashPlan MemFs::plan(Rng& rng) const {
    CrashPlan p;
    for (const auto& [path, d] : dirs_) {
        std::vector<bool> keep;
        for (std::size_t i = 0; i < d.pending.size(); ++i)
            keep.push_back(rng.below(2) == 1);
        std::map<std::string, int> after = apply_kept(d.dur, d.pending, &keep);
        p.dir_keep[path] = keep;
        for (const auto& [n, ino] : after) {
            const File& f = files_[std::size_t(ino)];
            if (f.data == f.dur)
                continue;
            FileFate fate;
            std::uint64_t common = common_prefix(f.data, f.dur);
            if (rng.below(4) == 0) {
                fate.revert = true;
            } else {
                std::uint64_t written = f.data.size() - common;
                fate.keep = rng.below(written + 1);
                if (fate.keep > 0 && rng.below(2) == 0) {
                    fate.bad_from = common + rng.below(fate.keep);
                    fate.bad_to = fate.bad_from + 1 + rng.below(common + fate.keep - fate.bad_from);
                }
            }
            p.files[path + "/" + n] = fate;
        }
    }
    return p;
}

MemFs::CrashPlan MemFs::plan_all(bool all) const {
    CrashPlan p;
    for (const auto& [path, d] : dirs_) {
        std::vector<bool> keep(d.pending.size(), all);
        std::map<std::string, int> after = apply_kept(d.dur, d.pending, &keep);
        p.dir_keep[path] = keep;
        for (const auto& [n, ino] : after) {
            const File& f = files_[std::size_t(ino)];
            FileFate fate;
            fate.revert = !all;
            fate.keep = all ? f.data.size() - common_prefix(f.data, f.dur) : 0;
            p.files[path + "/" + n] = fate;
        }
    }
    return p;
}

void MemFs::crash(const CrashPlan& plan) {
    for (auto& [path, d] : dirs_) {
        auto k = plan.dir_keep.find(path);
        std::map<std::string, int> after = apply_kept(d.dur, d.pending, k == plan.dir_keep.end() ? nullptr : &k->second);
        d.now = after;
        d.dur = after;
        d.pending.clear();
    }
    std::vector<bool> done(files_.size(), false);
    for (auto& [path, d] : dirs_) {
        for (const auto& [n, ino] : d.now) {
            if (done[std::size_t(ino)])
                continue;
            done[std::size_t(ino)] = true;
            File& f = files_[std::size_t(ino)];
            auto fate = plan.files.find(path + "/" + n);
            if (fate == plan.files.end() || fate->second.revert) {
                f.data = f.dur;
                continue;
            }
            std::uint64_t common = common_prefix(f.data, f.dur);
            std::uint64_t end = common + std::min<std::uint64_t>(fate->second.keep, f.data.size() - common);
            Bytes out(f.data.begin(), f.data.begin() + std::ptrdiff_t(end));
            for (std::uint64_t i = std::max(fate->second.bad_from, common); i < std::min(fate->second.bad_to, end); ++i)
                out[i] ^= 0xA5;
            f.data = out;
            f.dur = out;
        }
    }
    dead_ = false;
    fail_at_ = 0;
    collect();
}

bool MemFs::has(const std::string& path) const {
    return file(path) != nullptr;
}

const Bytes& MemFs::data(const std::string& path) const {
    static const Bytes kNone;
    const File* f = file(path);
    return f == nullptr ? kNone : f->data;
}

const Bytes& MemFs::durable(const std::string& path) const {
    static const Bytes kNone;
    const File* f = file(path);
    return f == nullptr ? kNone : f->dur;
}

const std::vector<MemFs::DirOp>& MemFs::pending(const std::string& dir) const {
    static const std::vector<DirOp> kNone;
    auto d = dirs_.find(dir);
    return d == dirs_.end() ? kNone : d->second.pending;
}

std::map<std::string, std::uint64_t> MemFs::sizes(const std::string& dir) const {
    std::map<std::string, std::uint64_t> out;
    auto d = dirs_.find(dir);
    if (d == dirs_.end())
        return out;
    for (const auto& [n, ino] : d->second.now)
        out[n] = files_[std::size_t(ino)].data.size();
    return out;
}

void MemFs::corrupt(const std::string& path, std::uint64_t off) {
    File* f = file(path);
    if (f == nullptr)
        return;
    if (off < f->data.size())
        f->data[off] ^= 0x5A;
    if (off < f->dur.size())
        f->dur[off] ^= 0x5A;
}

} // namespace nexus::store
