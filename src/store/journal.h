#pragma once
#include "store/codec.h"
#include "store/fs.h"
#include "store/status.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nexus::store {

// A crash-safe record log in one directory: an append-only log of checksummed, numbered records, and a snapshot that
// stands for every record up to its number. The owner keeps its state in memory, appends a record per change, syncs
// before it acknowledges anything, and now and then hands the journal a snapshot of its whole state so the log can
// start again empty. models/store/Journal.tla is the layout this file implements, step for step; the trace walk
// (tests/store/journal_walk.cpp) checks that it does.
//
// On disk, in `dir`:
//   log            records, back to back. A record: u32 n (payload bytes, at least 1), u32 crc, u64 seq, then the
//                  payload; crc is CRC-32C of n, seq and the payload. Numbers are little-endian. Records are numbered
//                  1, 2, 3, ... with no gaps across the snapshot and the log.
//   snapshot       u32 magic "NXSN", u32 format (1), u64 seq, u32 n, u32 crc, then n payload bytes; crc is CRC-32C of
//                  the header's first 20 bytes and the payload. It stands for records 1..seq. Missing: seq 0.
//   snapshot.tmp, log.new   a compaction in progress; removed by open.
//
// Open recovers: it removes the temporaries, checks the snapshot (written whole and synced before it got its name,
// so a damaged one is a disk fault: Failed), then reads the log from the start and keeps the longest run of whole,
// checksummed records numbered one after another. Whatever follows that run is a torn tail (bytes written and never
// synced, cut short or damaged by the crash): open truncates the log there and syncs it, so that the next record
// starts at a clean end. Records the snapshot covers are skipped; the first record after the snapshot must be
// seq + 1 (a gap is a disk fault: Failed).
//
// Durability: append() keeps the record in memory and writes nothing; sync() writes every record appended since the
// last sync to the log in one write, then syncs it, so that a batch of records costs one write and one flush. A
// record is acknowledged only after sync() returns. The log is opened once (Fs::open_append) and kept open; it is
// opened again after open() has cut it and after compaction has put a new log under its name. In the model, the one
// write is the batch's AppendRec steps, one per record, taken together (the trace walk binds them so). A crash before
// the write loses the batch, which nobody was told of; a crash between the write and the flush may keep any prefix of
// it, torn, as Journal.tla's crash already allows.
//
// compact() first syncs the log, then writes the snapshot under its temporary name, syncs it, renames it into place
// and syncs the directory, and only then replaces the log by an empty one (create log.new, rename it over log, sync
// the directory). A crash anywhere in that leaves either the old snapshot with the whole old log, or the new snapshot
// with the old log or the empty one; the records the snapshot covers are skipped on replay.
//
// After any failed write or sync the journal refuses every later call: what reached the disk is unknown until the
// process restarts and open() reads it back.
class Journal {
public:
    static constexpr std::uint32_t kRecordHeader = 16;
    static constexpr std::uint32_t kSnapshotHeader = 24;

    // Test-only faults, for the trace binding's negative checks (each must be rejected by TLC):
    //   AcceptTorn  open keeps a record whose checksum fails when its length fits
    //   CutTooFar   open cuts a torn tail one whole record early
    //   NoTruncate  open leaves the torn tail in place and appends after it
    //   NoSync      sync() writes the batch and reports success without syncing it: a record acknowledged before it
    //               is durable
    enum class Fault { None, AcceptTorn, CutTooFar, NoTruncate, NoSync };

    // Told what open() found, before it truncates: the last record number (k), the snapshot's (s) and the bytes of
    // the log kept (cut). For the trace walk.
    class Observer {
    public:
        virtual ~Observer() = default;
        virtual void recovered(std::uint64_t k, std::uint64_t s, std::uint64_t cut) = 0;
    };

    struct Recovery {
        std::uint64_t snapshot_seq = 0;
        Bytes snapshot;                 // empty when there is none
        std::vector<Bytes> records;     // the records after the snapshot, seq snapshot_seq + 1 onward
        std::uint64_t cut_bytes = 0;    // the torn tail removed from the log
    };

    Journal(Fs& fs, std::string dir, Fault fault = Fault::None, Observer* observer = nullptr);

    Status open(Recovery& out);
    Status append(std::span<const std::uint8_t> payload);
    Status sync();
    Status compact(std::span<const std::uint8_t> snapshot);

    std::uint64_t last_seq() const { return last_; }
    std::uint64_t durable_seq() const { return durable_; }
    std::uint64_t log_bytes() const { return log_bytes_; }
    bool failed() const { return failed_; }
    const std::string& dir() const { return dir_; }

private:
    Status fail(Status s);
    Status check() const;
    std::string path(const char* name) const { return dir_ + "/" + name; }

    Fs& fs_;
    std::string dir_;
    Fault fault_;
    Observer* observer_;
    std::unique_ptr<AppendFile> log_;   // the log, open while the journal is
    Bytes unwritten_;                   // the records appended since the last sync, back to back
    std::uint64_t last_ = 0;
    std::uint64_t durable_ = 0;
    std::uint64_t log_bytes_ = 0;
    bool open_ = false;
    bool failed_ = false;
};

} // namespace nexus::store
