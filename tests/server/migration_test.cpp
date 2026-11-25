// Unit tests of layer 5 migration and rollback-format checks (src/server/migration.h, src/server/build_manifest.h;
// models/migration/Migration.tla). Pure: MemFs only, no real time, no sleeps, no sockets.

#include "server/build_manifest.h"
#include "server/migration.h"
#include "store/fs.h"
#include "store/store.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace nexus;
using namespace nexus::server;
using namespace nexus::server::builds;
using namespace nexus::server::migration;
using store::Bytes;
using store::Change;
using store::Entry;
using store::FileLock;
using store::Info;
using store::LockMode;
using store::MemFs;
using store::Rng;
using store::Status;
using store::Store;
using store::StoreOptions;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_OK(expr)                                                                                 \
    do {                                                                                               \
        const Status _s = (expr);                                                                      \
        if (!_s.is_ok()) {                                                                             \
            std::fprintf(stderr, "%s:%d: not ok (%s): %s\n", __FILE__, __LINE__, #expr,                \
                         _s.message().c_str());                                                        \
            ++failures;                                                                                \
        }                                                                                              \
    } while (0)

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::span<const std::uint8_t> bytesOf(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string strOf(const Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

void writeDurable(MemFs& fs, const std::string& path, const std::string& text) {
    CHECK_OK(fs.make_dir(store::dir_of(path)));
    CHECK_OK(fs.write_new(path, bytesOf(text)));
    CHECK_OK(fs.sync_dir(store::dir_of(path)));
}

// Populates an old ~/.nexus layout on `fs`: a compacted store (both snapshot and log), drivers, config, server.log,
// a stale server.sock, and a sibling ~/.nexus/client/config that migration must never touch.
void populateOldHome(MemFs& fs) {
    CHECK_OK(fs.make_dir("/h/.nexus"));
    StoreOptions small;
    small.compact_bytes = 64;
    {
        std::unique_ptr<Store> s;
        CHECK_OK(Store::open(fs, "/h/.nexus/store", small, s));
        Change mk;
        mk.kind = Change::Kind::SetInfo;
        mk.info = Info{"ada", "claude", "main", "opus", "high", ""};
        Change msg;
        msg.kind = Change::Kind::Append;
        msg.entry = Entry{0, "keep this conversation across layout migration", true};
        CHECK_OK(s->commit(1, std::vector<Change>{mk, msg}, "w1", 1));
        Change q;
        q.kind = Change::Kind::Enqueue;
        q.prompt = "queued prompt";
        CHECK_OK(s->commit(1, std::vector<Change>{q}, "w1", 2));
    }
    CHECK(fs.has("/h/.nexus/store/snapshot") && fs.has("/h/.nexus/store/log"));
    writeDurable(fs, "/h/.nexus/drivers", "driver claude /usr/bin/node /d/main.js --account main\nai ada claude\n");
    writeDurable(fs, "/h/.nexus/config", "machine studio\n");
    writeDurable(fs, "/h/.nexus/server.log", "old server log\n");
    writeDurable(fs, "/h/.nexus/server.sock", "");
    writeDurable(fs, "/h/.nexus/client/config", "sprint-url https://board.example\n");
}

void verifyMigrated(MemFs& fs) {
    std::vector<std::string> topNames;
    CHECK_OK(fs.list("/h/.nexus", topNames));
    CHECK((topNames == std::vector<std::string>{"client", "server"}));
    CHECK(strOf(fs.data("/h/.nexus/client/config")) == "sprint-url https://board.example\n");
    CHECK(!fs.has("/h/.nexus/server/.migrating"));
    CHECK(has(strOf(fs.data("/h/.nexus/server/drivers")), "AI ada claude"));
    CHECK(strOf(fs.data("/h/.nexus/server/config")) == "machine studio\n");

    std::unique_ptr<Store> s;
    CHECK_OK(Store::open(fs, "/h/.nexus/server/store", StoreOptions{}, s));
    if (s != nullptr) {
        CHECK(s->sessions().size() == 1 && s->next_session() == 2 && s->applied("w1") == 2);
        const store::Session* ses = s->session(1);
        CHECK(ses != nullptr && ses->info.ai_name == "ada" && ses->transcript.size() == 1 &&
              ses->transcript[0].text == "keep this conversation across layout migration" &&
              ses->queue == std::vector<std::string>{"queued prompt"});
    }
}

void testMove() {
    // Empty home or explicit --dir (empty top): zero mutating calls.
    {
        MemFs fs;
        const std::uint64_t before = fs.calls();
        Report r;
        CHECK_OK(migrateOldLayout(fs, "", "/custom/dir", {}, &r));
        CHECK(!r.migrated && fs.calls() == before);
        CHECK_OK(migrateOldLayout(fs, "/h/.nexus", "/h/.nexus/server", {}, &r));
        CHECK(!r.migrated && fs.calls() == before);
    }

    // Full old layout moves once, leaves ~/.nexus/client untouched, and a second call makes zero writes.
    MemFs fs;
    populateOldHome(fs);
    Report r1;
    CHECK_OK(migrateOldLayout(fs, "/h/.nexus", "/h/.nexus/server", {}, &r1));
    CHECK(r1.migrated && !r1.resumed);
    verifyMigrated(fs);

    const std::uint64_t callsAfter = fs.calls();
    Report r2;
    CHECK_OK(migrateOldLayout(fs, "/h/.nexus", "/h/.nexus/server", {}, &r2));
    CHECK(!r2.migrated && fs.calls() == callsAfter);

    // Pre-existing server.log (made by `nexus-server new` when spawning `serve`) does not block migration.
    MemFs spawned;
    populateOldHome(spawned);
    writeDurable(spawned, "/h/.nexus/server/server.log", "spawned stderr\n");
    CHECK_OK(migrateOldLayout(spawned, "/h/.nexus", "/h/.nexus/server"));
    CHECK(strOf(spawned.data("/h/.nexus/server/server.log")) == "spawned stderr\n");
    verifyMigrated(spawned);
}

void testRefusals() {
    // 1. Live old server answering on ~/.nexus/server.sock: refused without mutating anything.
    {
        MemFs fs;
        populateOldHome(fs);
        const std::uint64_t before = fs.calls();
        migration::Options o;
        o.old_server_live = [] { return true; };
        const Status s = migrateOldLayout(fs, "/h/.nexus", "/h/.nexus/server", o);
        CHECK(!s.is_ok() && has(s.message(), "server.sock"));
        CHECK(fs.calls() == before && fs.has("/h/.nexus/store/log") && !fs.has("/h/.nexus/server/store/log"));
    }

    // 2. Old store locked by a writer: Busy without mutating anything.
    {
        MemFs fs;
        populateOldHome(fs);
        std::unique_ptr<FileLock> hold;
        CHECK_OK(fs.lock("/h/.nexus/store", LockMode::Exclusive, hold));
        const std::uint64_t before = fs.calls();
        const Status s = migrateOldLayout(fs, "/h/.nexus", "/h/.nexus/server");
        CHECK(s.code() == Status::Code::Busy && fs.calls() == before);
    }

    // 3. Pre-existing populated ~/.nexus/server/store: NeverClobbersNewStore refuses and keeps both stores intact.
    {
        MemFs fs;
        populateOldHome(fs);
        {
            std::unique_ptr<Store> s;
            CHECK_OK(Store::open(fs, "/h/.nexus/server/store", StoreOptions{}, s));
            Change mk;
            mk.kind = Change::Kind::SetInfo;
            mk.info = Info{"bo", "claude", "main", "", "", ""};
            CHECK_OK(s->commit(1, std::vector<Change>{mk}, "w2", 1));
        }
        const Bytes newLog = fs.data("/h/.nexus/server/store/log");
        const Bytes oldLog = fs.data("/h/.nexus/store/log");
        const Status s = migrateOldLayout(fs, "/h/.nexus", "/h/.nexus/server");
        CHECK(s.code() == Status::Code::Failed && has(s.message(), "neither was overwritten"));
        CHECK(fs.data("/h/.nexus/server/store/log") == newLog);
        CHECK(fs.data("/h/.nexus/store/log") == oldLog);
    }

    // 4. Conflicting drivers file in ~/.nexus/server: refused, keeping both; identical drivers file: cleaned cleanly.
    {
        MemFs diff;
        populateOldHome(diff);
        writeDurable(diff, "/h/.nexus/server/drivers", "AI other claude\n");
        const Status s = migrateOldLayout(diff, "/h/.nexus", "/h/.nexus/server");
        CHECK(s.code() == Status::Code::Failed && has(s.message(), "drivers"));
        CHECK(strOf(diff.data("/h/.nexus/server/drivers")) == "AI other claude\n");
        CHECK(diff.has("/h/.nexus/drivers"));

        MemFs same;
        populateOldHome(same);
        const std::string drv = strOf(same.data("/h/.nexus/drivers"));
        writeDurable(same, "/h/.nexus/server/drivers", drv);
        CHECK_OK(migrateOldLayout(same, "/h/.nexus", "/h/.nexus/server"));
        verifyMigrated(same);
    }

    // 5. Unexpected non-journal file in ~/.nexus/store: refused rather than deleting unknown user data.
    {
        MemFs fs;
        populateOldHome(fs);
        writeDurable(fs, "/h/.nexus/store/notes.txt", "keep me\n");
        const Status s = migrateOldLayout(fs, "/h/.nexus", "/h/.nexus/server");
        CHECK(s.code() == Status::Code::Failed && has(s.message(), "notes.txt"));
        CHECK(fs.has("/h/.nexus/store/notes.txt"));
    }
}

void testCrash() {
    MemFs base;
    populateOldHome(base);
    Rng rng(0x5eed1234ULL);

    // Crash at every single mutating filesystem call (drop-all, keep-all, and three random torn/partial plans each),
    // plus double-crash (kill-9 leaving volatile state followed by a power-cut mid-resume: the TLC counterexample!).
    std::uint64_t maxK = 0;
    for (std::uint64_t k = 1;; ++k) {
        MemFs probe(base);
        probe.fail_at(k);
        const Status st = migrateOldLayout(probe, "/h/.nexus", "/h/.nexus/server");
        if (!probe.crashed()) {
            CHECK_OK(st);
            maxK = k - 1;
            break;
        }
        for (int mode = 0; mode < 5; ++mode) {
            MemFs f(probe);
            if (mode == 0) {
                f.crash(f.plan_all(false));
            } else if (mode == 1) {
                f.crash(f.plan_all(true));
            } else {
                f.crash(f.plan(rng));
            }
            CHECK_OK(migrateOldLayout(f, "/h/.nexus", "/h/.nexus/server"));
            f.crash(f.plan_all(false));
            verifyMigrated(f);
        }
        // Kill -9 (plan_all(true): volatile OS cache kept, unsynced on disk), then resume cut again by a power loss!
        for (std::uint64_t k2 = 1; k2 <= 8; ++k2) {
            MemFs f(probe);
            f.crash(f.plan_all(true));
            // Simulate unsynced .migrating still in page cache ifprobe had written it before sync_dir: tested directly
            // by cutting the resuming run at k2 with plan_all(false).
            f.fail_at(k2);
            (void)migrateOldLayout(f, "/h/.nexus", "/h/.nexus/server");
            f.crash(f.plan_all(false));
            CHECK_OK(migrateOldLayout(f, "/h/.nexus", "/h/.nexus/server"));
            f.crash(f.plan_all(false));
            verifyMigrated(f);
        }
    }
    CHECK(maxK >= 15);
}

void testFormats() {
    const std::string h1(64, 'a');
    Formats b1;
    std::string why;
    CHECK(readFormats("format store-record 3 3\nformat store-snapshot 4 4\nformat config 2 2\n", &b1, &why));

    // Empty disk: readable by b1.
    DiskFormats d;
    CHECK(diskFormatsRefusal(b1, h1, d).empty());

    // Compatible later store (record reader 3, snapshot reader 4, config 2): readable by b1.
    d = DiskFormats{3, 9, 4, 2, "accounts.json"};
    CHECK(diskFormatsRefusal(b1, h1, d).empty());

    // Breaking store record (reader 5 > b1.writes 3): refused, naming the record and versions.
    d = DiskFormats{5, 4, 4, 2, "accounts.json"};
    std::string ref = diskFormatsRefusal(b1, h1, d);
    CHECK(has(ref, "store-record") && has(ref, "record 4") && has(ref, "5") && has(ref, h1));

    // Breaking snapshot (reader 6 > b1.writes 4): refused.
    d = DiskFormats{3, 1, 6, 2, "accounts.json"};
    ref = diskFormatsRefusal(b1, h1, d);
    CHECK(has(ref, "store-snapshot") && has(ref, "6"));

    // Breaking config (version 3 > b1.writes 2): refused.
    d = DiskFormats{3, 1, 4, 3, "ais.json"};
    ref = diskFormatsRefusal(b1, h1, d);
    CHECK(has(ref, "config") && has(ref, "ais.json") && has(ref, "3"));

    // readDiskFormats over MemFs: reads store readers and nexus-config JSON versions without mutating disk.
    MemFs fs;
    {
        std::unique_ptr<Store> s;
        CHECK_OK(Store::open(fs, "/srv/store", StoreOptions{}, s));
        Change mk;
        mk.kind = Change::Kind::SetInfo;
        mk.info = Info{"ada", "claude", "main", "", "", ""};
        CHECK_OK(s->commit(1, std::vector<Change>{mk}, "w1", 1));
    }
    writeDurable(fs, "/srv/nexus-config/accounts.json", "{\n  \"version\": 2,\n  \"accounts\": []\n}\n");
    writeDurable(fs, "/srv/nexus-config/ais.json", "{\n  \"version\": 3,\n  \"ais\": []\n}\n");
    DiskFormats got;
    const std::uint64_t callsBefore = fs.calls();
    CHECK_OK(readDiskFormats(fs, "/srv", &got));
    CHECK(fs.calls() == callsBefore);
    CHECK(got.storeRecordReader == 3 && got.storeRecordSeq == 1 && got.configVersion == 3 &&
          got.configFile == "ais.json");
    CHECK(has(diskFormatsRefusal(b1, h1, got), "ais.json"));
}

} // namespace

int main(int argc, char** argv) {
    const std::string g = argc > 1 ? argv[1] : "";
    if (argc > 2) {
        std::fprintf(stderr, "usage: migration_test [move|refusals|crash|formats]\n");
        return 2;
    }
    if (g.empty()) {
        testMove();
        testRefusals();
        testCrash();
        testFormats();
    } else if (g == "move") {
        testMove();
    } else if (g == "refusals") {
        testRefusals();
    } else if (g == "crash") {
        testCrash();
    } else if (g == "formats") {
        testFormats();
    } else {
        std::fprintf(stderr, "unknown group: %s\n", g.c_str());
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("migration_test%s%s: all checks passed\n", g.empty() ? "" : " ", g.c_str());
    return 0;
}
