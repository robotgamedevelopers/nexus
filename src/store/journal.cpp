// The journal: models/store/Journal.tla, step for step. Each filesystem call below is one action of that spec; the
// comments name it.

#include "store/journal.h"

#include <algorithm>

namespace nexus::store {

namespace {

constexpr std::uint32_t kMagic = 0x4E53584E;  // "NXSN"
constexpr std::uint32_t kFormat = 1;

Bytes record_bytes(std::uint64_t seq, std::span<const std::uint8_t> payload) {
    Bytes out(Journal::kRecordHeader + payload.size());
    put_u32(out.data(), std::uint32_t(payload.size()));
    put_u64(out.data() + 8, seq);
    std::copy(payload.begin(), payload.end(), out.begin() + Journal::kRecordHeader);
    std::uint32_t crc = crc32c(std::span<const std::uint8_t>(out.data(), 4));
    crc = crc32c(std::span<const std::uint8_t>(out.data() + 8, out.size() - 8), crc);
    put_u32(out.data() + 4, crc);
    return out;
}

Bytes snapshot_bytes(std::uint64_t seq, std::span<const std::uint8_t> payload) {
    Bytes out(Journal::kSnapshotHeader + payload.size());
    put_u32(out.data(), kMagic);
    put_u32(out.data() + 4, kFormat);
    put_u64(out.data() + 8, seq);
    put_u32(out.data() + 16, std::uint32_t(payload.size()));
    std::copy(payload.begin(), payload.end(), out.begin() + Journal::kSnapshotHeader);
    std::uint32_t crc = crc32c(std::span<const std::uint8_t>(out.data(), 20));
    crc = crc32c(std::span<const std::uint8_t>(out.data() + Journal::kSnapshotHeader, payload.size()), crc);
    put_u32(out.data() + 20, crc);
    return out;
}

bool has(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

Journal::Journal(Fs& fs, std::string dir, Fault fault, Observer* observer)
    : fs_(fs), dir_(std::move(dir)), fault_(fault), observer_(observer) {}

Status Journal::fail(Status s) {
    if (!s.is_ok())
        failed_ = true;
    return s;
}

Status Journal::check() const {
    if (failed_)
        return Status::failed("journal " + dir_ + ": an earlier write or sync failed, so what is on disk is unknown; "
                              "restart the server to reopen the store from what is durable");
    if (!open_)
        return Status::failed("journal " + dir_ + ": not open");
    return Status::ok();
}

Status Journal::open(Recovery& out) {
    out = Recovery{};
    last_ = durable_ = log_bytes_ = 0;
    open_ = false;
    failed_ = false;
    log_.reset();
    unwritten_.clear();
    Status s = fs_.make_dir(dir_);
    if (!s.is_ok())
        return fail(s);
    std::vector<std::string> names;
    s = fs_.list(dir_, names);
    if (!s.is_ok())
        return fail(s);

    // RemoveTmp: a compaction that did not finish.
    if (has(names, "snapshot.tmp") && !(s = fs_.remove(path("snapshot.tmp"))).is_ok())
        return fail(s);
    if (has(names, "log.new") && !(s = fs_.remove(path("log.new"))).is_ok())
        return fail(s);

    // Recover: the snapshot, then the longest run of whole records in the log.
    std::uint64_t snap_seq = 0;
    if (has(names, "snapshot")) {
        Bytes b;
        if (!(s = fs_.read_all(path("snapshot"), b)).is_ok())
            return fail(s);
        bool whole = b.size() >= kSnapshotHeader && get_u32(b.data()) == kMagic && get_u32(b.data() + 4) == kFormat &&
                     get_u32(b.data() + 16) == b.size() - kSnapshotHeader;
        if (whole) {
            std::uint32_t crc = crc32c(std::span<const std::uint8_t>(b.data(), 20));
            crc = crc32c(std::span<const std::uint8_t>(b.data() + kSnapshotHeader, b.size() - kSnapshotHeader), crc);
            whole = crc == get_u32(b.data() + 20);
        }
        if (!whole)
            return fail(Status::failed(
                "journal " + dir_ + ": the snapshot is damaged (it is synced before it is named, so this is a disk "
                "fault, not a crash); restore the store directory from a backup"));
        snap_seq = get_u64(b.data() + 8);
        out.snapshot.assign(b.begin() + kSnapshotHeader, b.end());
    }
    out.snapshot_seq = snap_seq;

    if (!has(names, "log")) {
        if (has(names, "snapshot"))
            return fail(Status::failed("journal " + dir_ + ": the snapshot is there but the log is missing; restore "
                                       "the store directory from a backup"));
        // A new journal: the log, durably named.
        if (!(s = fs_.create(path("log"))).is_ok() || !(s = fs_.sync_dir(dir_)).is_ok())
            return fail(s);
    }

    Bytes log;
    if (!(s = fs_.read_all(path("log"), log)).is_ok())
        return fail(s);
    std::uint64_t at = 0;
    std::uint64_t prev = 0;
    std::uint64_t last_at = 0;
    std::uint64_t first_after = 0;
    std::uint64_t count = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;  // (offset, seq) of each record kept
    while (log.size() - at >= kRecordHeader) {
        const std::uint8_t* p = log.data() + at;
        std::uint32_t n = get_u32(p);
        if (n == 0 || n > log.size() - at - kRecordHeader)
            break;
        std::uint32_t crc = crc32c(std::span<const std::uint8_t>(p, 4));
        crc = crc32c(std::span<const std::uint8_t>(p + 8, 8 + std::size_t(n)), crc);
        if (crc != get_u32(p + 4) && fault_ != Fault::AcceptTorn)
            break;
        std::uint64_t seq = get_u64(p + 8);
        if (count > 0 && seq != prev + 1)
            break;
        spans.emplace_back(at, seq);
        prev = seq;
        last_at = at;
        at += kRecordHeader + n;
        ++count;
    }
    std::uint64_t cut = at;
    if (fault_ == Fault::CutTooFar && cut < log.size() && !spans.empty()) {
        cut = last_at;
        spans.pop_back();
        prev = spans.empty() ? 0 : spans.back().second;
    }
    // The decision: the last record is the log's last whole one, or the snapshot's when the log holds nothing after it.
    // It goes to the observer before it is checked, so that a trace shows a faulty decision even when the checks below
    // then refuse the disk it leaves.
    for (const auto& [off, seq] : spans) {
        if (seq > snap_seq && first_after == 0)
            first_after = seq;
    }
    std::uint64_t last = first_after == 0 ? snap_seq : prev;
    if (observer_ != nullptr)
        observer_->recovered(last, snap_seq, cut);

    if (!spans.empty() && prev < snap_seq)
        return fail(Status::failed("journal " + dir_ + ": the log ends at record " + std::to_string(prev) +
                                   " but the snapshot stands for records to " + std::to_string(snap_seq) +
                                   "; restore the store directory from a backup"));
    if (first_after != 0 && first_after != snap_seq + 1)
        return fail(Status::failed("journal " + dir_ + ": records " + std::to_string(snap_seq + 1) + " to " +
                                   std::to_string(first_after - 1) + " are missing; restore the store directory "
                                   "from a backup"));
    for (const auto& [off, seq] : spans) {
        if (seq <= snap_seq)
            continue;
        std::uint32_t n = get_u32(log.data() + off);
        out.records.emplace_back(log.begin() + std::ptrdiff_t(off + kRecordHeader),
                                 log.begin() + std::ptrdiff_t(off + kRecordHeader + n));
    }
    last_ = last;
    durable_ = last_;
    out.cut_bytes = log.size() - cut;

    // Truncate, then sync it: the next record starts at a clean end.
    if (cut < log.size() && fault_ != Fault::NoTruncate) {
        if (!(s = fs_.truncate(path("log"), cut)).is_ok() || !(s = fs_.sync(path("log"))).is_ok())
            return fail(s);
        log_bytes_ = cut;
    } else {
        log_bytes_ = log.size();
    }
    // The log, open from here on: every batch is written and flushed through it.
    if (!(s = fs_.open_append(path("log"), log_)).is_ok()) {
        return fail(s);
    }
    open_ = true;
    return Status::ok();
}

Status Journal::append(std::span<const std::uint8_t> payload) {
    Status s = check();
    if (!s.is_ok())
        return s;
    if (payload.empty())
        return Status::refused("journal " + dir_ + ": a record needs at least one byte");
    // The record waits in memory for the batch's one write (sync).
    Bytes rec = record_bytes(last_ + 1, payload);
    unwritten_.insert(unwritten_.end(), rec.begin(), rec.end());
    ++last_;
    log_bytes_ += rec.size();
    return Status::ok();
}

Status Journal::sync() {
    Status s = check();
    if (!s.is_ok())
        return s;
    if (durable_ == last_)
        return Status::ok();
    // AppendRec, once per record since the last sync: the batch at the end of the log in one write, not yet durable.
    if (!unwritten_.empty()) {
        if (!(s = log_->append(unwritten_)).is_ok()) {
            return fail(s);
        }
        unwritten_.clear();
    }
    // SyncFile("log"): every record appended so far is durable.
    if (fault_ != Fault::NoSync && !(s = log_->sync()).is_ok()) {
        return fail(s);
    }
    durable_ = last_;
    return Status::ok();
}

Status Journal::compact(std::span<const std::uint8_t> snapshot) {
    Status s = sync();
    if (!s.is_ok())
        return s;
    Bytes b = snapshot_bytes(last_, snapshot);
    // CreateSnapTmp, AppendSnap, SyncFile("snapshot.tmp"), RenameSnap, SyncDir: the snapshot, durably named.
    if (!(s = fs_.create(path("snapshot.tmp"))).is_ok() || !(s = fs_.append(path("snapshot.tmp"), b)).is_ok() ||
        !(s = fs_.sync(path("snapshot.tmp"))).is_ok() ||
        !(s = fs_.rename(path("snapshot.tmp"), path("snapshot"))).is_ok() || !(s = fs_.sync_dir(dir_)).is_ok())
        return fail(s);
    // CreateLogNew, RenameLog, SyncDir: the log starts again, empty.
    if (!(s = fs_.create(path("log.new"))).is_ok() || !(s = fs_.rename(path("log.new"), path("log"))).is_ok() ||
        !(s = fs_.sync_dir(dir_)).is_ok())
        return fail(s);
    // The open log is the old one, no longer named: open the new one.
    if (!(s = fs_.open_append(path("log"), log_)).is_ok()) {
        return fail(s);
    }
    log_bytes_ = 0;
    return Status::ok();
}

} // namespace nexus::store
