// Unit tests of nexus's formats as schema's variable tables (schema/Nexus.schema, its generated code in schema/gen).
// In memory only: no time, no sockets, no files.
//
//   wire_test             runs every group
//   wire_test <group>     runs one:
//     roundtrip   every table, as a root or inside one (and every arm of the store's Change), written and read back
//                 field for field with a silent report; the same value written twice gives the same bytes; a table
//                 with nothing set reads every default; bytes cut short are refused, never half read
//     versions    a window and a server of different versions read each other's bytes, both ways: this build's
//                 tables against a later version's (tests/wire/next/NexusNext.schema), which adds a field, an enum
//                 value and a union arm. The older reader skips and counts what it does not know, reads the unknown
//                 kind and the unknown arm as None, and keeps everything else; the newer reader reads what the older
//                 writer never wrote as its default. Nothing is refused either way.

#include "versions.h"

#include "wire/tables.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace nexuswire;
using wiretest::Bytes;
using wiretest::Read;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

void setText(TableWorker& worker, TableRef& slot, const std::string& text) {
    TableStringEmplace(worker, slot, text.data(), static_cast<int64_t>(text.size()));
}

std::string textOf(const TableRef& ref) {
    const TableStringView view = TableStringAt(ref);
    return view.data == nullptr ? std::string() : std::string(view.data, static_cast<std::size_t>(view.length));
}

bool absent(const TableRef& ref) {
    return TableStringAt(ref).data == nullptr;
}

// A list of text's element i, through the generated accessor `at` (SessionChangeQueueTextAt and its kin).
template <typename T, typename At>
std::string textAt(const T& value, int32_t i, At at) {
    int32_t length = 0;
    if (!at(value, i, nullptr, 0, &length, nullptr)) {
        return "<missing>";
    }
    std::string text(static_cast<std::size_t>(length), '\0');
    at(value, i, reinterpret_cast<std::uint8_t*>(text.data()), length, &length, nullptr);
    return text;
}

// Locks a builder and saves it; empty when either fails.
template <typename Builder, typename Measure, typename Save>
Bytes saved(Builder& b, Measure measure, Save save) {
    if (!b.Lock()) {
        return Bytes();
    }
    const int64_t need = measure(b);
    if (need <= 0) {
        return Bytes();
    }
    Bytes out(static_cast<std::size_t>(need));
    const int64_t wrote = save(b, out.data(), need);
    out.resize(wrote < 0 ? 0 : static_cast<std::size_t>(wrote));
    return out;
}

// Reads `bytes` into `region`; null when refused. The report is left in `report`.
template <typename T, typename Measure, typename Load>
const T* loaded(const Bytes& bytes, std::vector<std::uint8_t>& region, TableReport& report, Measure measure,
                Load load) {
    const int64_t need = measure(bytes.data(), static_cast<int64_t>(bytes.size()));
    if (need < 0) {
        report.refused = true;
        return nullptr;
    }
    region.assign(static_cast<std::size_t>(need), 0);
    return load(region.data(), need, bytes.data(), static_cast<int64_t>(bytes.size()), &report);
}

bool silent(const TableReport& r) {
    return r.unknown == 0 && r.widened == 0 && r.missing_required == 0 && !r.damaged && !r.refused;
}

// ---- each root, built ----

Bytes helloBytes() {
    HelloBuilder b;
    b.GetRoot()->proto = 3;
    b.GetRoot()->min_peer = 2;
    return saved(b, [](const HelloBuilder& x) { return HelloMeasure(x); },
        [](const HelloBuilder& x, std::uint8_t* out, int64_t n) { return HelloSave(x, out, n); });
}

void fillInfo(TableWorker& w, SessionInfo& info, const std::string& tag) {
    setText(w, info.friend_name, "friend-" + tag);
    setText(w, info.harness, "claude");
    setText(w, info.account, "account-" + tag);
    setText(w, info.model, "model-" + tag);
    setText(w, info.effort, "high");
    setText(w, info.instance, "instance-" + tag);
}

void checkInfo(const SessionInfo& info, const std::string& tag) {
    CHECK(textOf(info.friend_name) == "friend-" + tag);
    CHECK(textOf(info.harness) == "claude");
    CHECK(textOf(info.account) == "account-" + tag);
    CHECK(textOf(info.model) == "model-" + tag);
    CHECK(textOf(info.effort) == "high");
    CHECK(textOf(info.instance) == "instance-" + tag);
}

// Two sessions: one unchanged (its id alone), one changed with every field, each entry kind, an entry that grew
// (offset non-zero, open) and a queue; and an empty string, which is present and empty, never absent.
Bytes sessionsBytes() {
    SessionsDiffBuilder b;
    SessionsDiff* d = b.GetRoot();
    SessionChange* kept = SessionsDiffSessionsAdd(b.main, d->sessions);
    kept->id = 1;
    SessionChange* s = SessionsDiffSessionsAdd(b.main, d->sessions);
    s->id = 2;
    s->changed = true;
    fillInfo(b.main, s->info, "a");
    s->activity = Activity::Down;
    s->first = 40;
    s->keep_from = 41;
    const EntryKind kinds[] = {EntryKind::Person, EntryKind::Friend, EntryKind::Notice, EntryKind::Tool};
    for (EntryKind k : kinds) {
        EntryChange* e = SessionChangeEntriesAdd(b.main, s->entries);
        e->kind = k;
        e->closed = true;
        setText(b.main, e->tail, std::string("text of ") + EnumName(k));
    }
    EntryChange* grew = SessionChangeEntriesAdd(b.main, s->entries);
    grew->kind = EntryKind::Friend;
    grew->closed = false;
    grew->offset = 1234;
    setText(b.main, grew->tail, "");
    s->queue_changed = true;
    SessionChangeQueueAppend(b.main, *s, "first");
    SessionChangeQueueAppend(b.main, *s, "second, with \xc3\xa9");
    return saved(b, [](const SessionsDiffBuilder& x) { return SessionsDiffMeasure(x); },
        [](const SessionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return SessionsDiffSave(x, out, n); });
}

// Every action kind, numbered from 9.
Bytes actionsBytes() {
    ActionsDiffBuilder b;
    ActionsDiff* d = b.GetRoot();
    d->start = 9;
    for (int k = static_cast<int>(ActionKind::Send); k <= static_cast<int>(ActionKind::Max); ++k) {
        Action* a = ActionsDiffActionsAdd(b.main, d->actions);
        a->kind = static_cast<ActionKind>(k);
        a->session = static_cast<std::uint64_t>(100 + k);
        setText(b.main, a->friend_name, "friend " + std::to_string(k));
        setText(b.main, a->text, "text " + std::to_string(k));
        setText(b.main, a->account, "account " + std::to_string(k));
    }
    return saved(b, [](const ActionsDiffBuilder& x) { return ActionsDiffMeasure(x); },
        [](const ActionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return ActionsDiffSave(x, out, n); });
}

// One change of every arm, and the client's counter.
Bytes recordBytes() {
    RecordBuilder b;
    Record* r = b.GetRoot();
    r->session = 5;
    Change* c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::Append;
    c->append = StoreEntry{};
    c->append.kind = EntryKind::Tool;
    c->append.closed = true;
    setText(b.main, c->append.text, "ran ls");
    c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::Enqueue;
    c->enqueue = TableRef{};
    setText(b.main, c->enqueue, "queued");
    c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::SetDriver;
    c->set_driver = Driver{};
    c->set_driver.next_turn = 8;
    c->set_driver.open_turn = 7;
    setText(b.main, c->set_driver.session, "harness-session");
    DriverWaitingAppend(b.main, c->set_driver, "waiting one");
    DriverWaitingAppend(b.main, c->set_driver, "waiting two");
    c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::Dequeue;
    c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::SetInfo;
    c->set_info = SessionInfo{};
    fillInfo(b.main, c->set_info, "r");
    setText(b.main, r->client, "window-1");
    r->applied = 77;
    return saved(b, [](const RecordBuilder& x) { return RecordMeasure(x); },
        [](const RecordBuilder& x, std::uint8_t* out, int64_t n) { return RecordSave(x, out, n); });
}

// Two sessions, each with a transcript, a queue and a driver, and two clients.
Bytes snapshotBytes() {
    SnapshotBuilder b;
    Snapshot* s = b.GetRoot();
    s->next_session = 3;
    for (std::uint64_t id = 1; id <= 2; ++id) {
        const std::string tag = std::to_string(id);
        StoreSession* x = SnapshotSessionsAdd(b.main, s->sessions);
        x->id = id;
        fillInfo(b.main, x->info, tag);
        for (int i = 0; i < 3; ++i) {
            StoreEntry* e = StoreSessionTranscriptAdd(b.main, x->transcript);
            e->kind = i == 0 ? EntryKind::Person : EntryKind::Friend;
            e->closed = i != 2;
            setText(b.main, e->text, "entry " + tag + "." + std::to_string(i));
        }
        StoreSessionQueueAppend(b.main, *x, ("queued " + tag).c_str());
        x->driver.next_turn = 10 * id;
        x->driver.open_turn = 0;
        setText(b.main, x->driver.session, "s-" + tag);
        DriverWaitingAppend(b.main, x->driver, ("waiting " + tag).c_str());
    }
    for (int i = 1; i <= 2; ++i) {
        ClientCounter* c = SnapshotClientsAdd(b.main, s->clients);
        setText(b.main, c->client, "window-" + std::to_string(i));
        c->applied = static_cast<std::uint64_t>(i * 1000);
    }
    return saved(b, [](const SnapshotBuilder& x) { return SnapshotMeasure(x); },
        [](const SnapshotBuilder& x, std::uint8_t* out, int64_t n) { return SnapshotSave(x, out, n); });
}

// ---- roundtrip ----

const auto helloMeasure = [](const std::uint8_t* in, int64_t n) { return HelloLoadMeasure(in, n); };
const auto helloLoad = [](std::uint8_t* r, int64_t rn, const std::uint8_t* in, int64_t n, TableReport* rep) {
    return HelloLoad(r, rn, in, n, rep);
};
const auto sessionsMeasure = [](const std::uint8_t* in, int64_t n) { return SessionsDiffLoadMeasure(in, n); };
const auto sessionsLoad = [](std::uint8_t* r, int64_t rn, const std::uint8_t* in, int64_t n, TableReport* rep) {
    return SessionsDiffLoad(r, rn, in, n, rep);
};
const auto actionsMeasure = [](const std::uint8_t* in, int64_t n) { return ActionsDiffLoadMeasure(in, n); };
const auto actionsLoad = [](std::uint8_t* r, int64_t rn, const std::uint8_t* in, int64_t n, TableReport* rep) {
    return ActionsDiffLoad(r, rn, in, n, rep);
};
const auto recordMeasure = [](const std::uint8_t* in, int64_t n) { return RecordLoadMeasure(in, n); };
const auto recordLoad = [](std::uint8_t* r, int64_t rn, const std::uint8_t* in, int64_t n, TableReport* rep) {
    return RecordLoad(r, rn, in, n, rep);
};
const auto snapshotMeasure = [](const std::uint8_t* in, int64_t n) { return SnapshotLoadMeasure(in, n); };
const auto snapshotLoad = [](std::uint8_t* r, int64_t rn, const std::uint8_t* in, int64_t n, TableReport* rep) {
    return SnapshotLoad(r, rn, in, n, rep);
};

void roundtripHello() {
    const Bytes bytes = helloBytes();
    CHECK(!bytes.empty());
    CHECK(bytes == helloBytes());
    std::vector<std::uint8_t> region;
    TableReport report;
    const Hello* h = loaded<Hello>(bytes, region, report, helloMeasure, helloLoad);
    CHECK(h != nullptr && silent(report));
    if (h != nullptr) {
        CHECK(h->proto == 3);
        CHECK(h->min_peer == 2);
    }
}

void roundtripSessions() {
    const Bytes bytes = sessionsBytes();
    CHECK(!bytes.empty());
    CHECK(bytes == sessionsBytes());
    std::vector<std::uint8_t> region;
    TableReport report;
    const SessionsDiff* d = loaded<SessionsDiff>(bytes, region, report, sessionsMeasure, sessionsLoad);
    CHECK(d != nullptr && silent(report));
    if (d == nullptr || d->sessions.size() != 2) {
        CHECK(false);
        return;
    }
    const SessionChange& kept = d->sessions[0];
    CHECK(kept.id == 1);
    CHECK(!kept.changed);
    CHECK(absent(kept.info.friend_name));
    CHECK(kept.activity == Activity::Idle);
    CHECK(kept.entries.size() == 0);
    CHECK(!kept.queue_changed);
    const SessionChange& s = d->sessions[1];
    CHECK(s.id == 2);
    CHECK(s.changed);
    checkInfo(s.info, "a");
    CHECK(s.activity == Activity::Down);
    CHECK(s.first == 40);
    CHECK(s.keep_from == 41);
    CHECK(s.entries.size() == 5);
    if (s.entries.size() == 5) {
        const EntryKind kinds[] = {EntryKind::Person, EntryKind::Friend, EntryKind::Notice, EntryKind::Tool};
        for (int32_t i = 0; i < 4; ++i) {
            const EntryChange& e = s.entries[i];
            CHECK(e.kind == kinds[i]);
            CHECK(e.closed);
            CHECK(e.offset == 0);
            CHECK(textOf(e.tail) == std::string("text of ") + EnumName(kinds[i]));
        }
        const EntryChange& grew = s.entries[4];
        CHECK(grew.kind == EntryKind::Friend);
        CHECK(!grew.closed);
        CHECK(grew.offset == 1234);
        CHECK(!absent(grew.tail));
        CHECK(textOf(grew.tail).empty());
    }
    CHECK(s.queue_changed);
    CHECK(s.queue.size() == 2);
    const auto at = [](const SessionChange& v, int32_t i, std::uint8_t* o, int64_t c, int32_t* l, bool* p) {
        return SessionChangeQueueTextAt(v, i, o, c, l, p);
    };
    CHECK(textAt(s, 0, at) == "first");
    CHECK(textAt(s, 1, at) == "second, with \xc3\xa9");
}

void roundtripActions() {
    const Bytes bytes = actionsBytes();
    CHECK(!bytes.empty());
    CHECK(bytes == actionsBytes());
    std::vector<std::uint8_t> region;
    TableReport report;
    const ActionsDiff* d = loaded<ActionsDiff>(bytes, region, report, actionsMeasure, actionsLoad);
    CHECK(d != nullptr && silent(report));
    if (d == nullptr) {
        return;
    }
    CHECK(d->start == 9);
    const int kinds = static_cast<int>(ActionKind::Max);
    CHECK(kinds == 21);     // through SignOut (20) and ResolveLimit (21)
    CHECK(d->actions.size() == kinds);
    for (int32_t i = 0; i < d->actions.size(); ++i) {
        const int k = i + 1;
        const Action& a = d->actions[i];
        CHECK(a.kind == static_cast<ActionKind>(k));
        CHECK(a.session == static_cast<std::uint64_t>(100 + k));
        CHECK(textOf(a.friend_name) == "friend " + std::to_string(k));
        CHECK(textOf(a.text) == "text " + std::to_string(k));
        CHECK(textOf(a.account) == "account " + std::to_string(k));
    }
}

void roundtripRecord() {
    const Bytes bytes = recordBytes();
    CHECK(!bytes.empty());
    CHECK(bytes == recordBytes());
    std::vector<std::uint8_t> region;
    TableReport report;
    const Record* r = loaded<Record>(bytes, region, report, recordMeasure, recordLoad);
    CHECK(r != nullptr && silent(report));
    if (r == nullptr || r->changes.size() != 5) {
        CHECK(false);
        return;
    }
    CHECK(r->session == 5);
    CHECK(textOf(r->client) == "window-1");
    CHECK(r->applied == 77);
    const Change& append = r->changes[0];
    CHECK(append.type == ChangeType::Append);
    if (append.type == ChangeType::Append) {
        CHECK(append.append.kind == EntryKind::Tool);
        CHECK(append.append.closed);
        CHECK(textOf(append.append.text) == "ran ls");
    }
    const Change& enqueue = r->changes[1];
    CHECK(enqueue.type == ChangeType::Enqueue);
    if (enqueue.type == ChangeType::Enqueue) {
        CHECK(textOf(enqueue.enqueue) == "queued");
    }
    const Change& driver = r->changes[2];
    CHECK(driver.type == ChangeType::SetDriver);
    if (driver.type == ChangeType::SetDriver) {
        CHECK(driver.set_driver.next_turn == 8);
        CHECK(driver.set_driver.open_turn == 7);
        CHECK(textOf(driver.set_driver.session) == "harness-session");
        CHECK(driver.set_driver.waiting.size() == 2);
        const auto at = [](const Driver& v, int32_t i, std::uint8_t* o, int64_t c, int32_t* l, bool* p) {
            return DriverWaitingTextAt(v, i, o, c, l, p);
        };
        CHECK(textAt(driver.set_driver, 0, at) == "waiting one");
        CHECK(textAt(driver.set_driver, 1, at) == "waiting two");
    }
    CHECK(r->changes[3].type == ChangeType::Dequeue);
    const Change& info = r->changes[4];
    CHECK(info.type == ChangeType::SetInfo);
    if (info.type == ChangeType::SetInfo) {
        checkInfo(info.set_info, "r");
    }
}

void roundtripSnapshot() {
    const Bytes bytes = snapshotBytes();
    CHECK(!bytes.empty());
    CHECK(bytes == snapshotBytes());
    std::vector<std::uint8_t> region;
    TableReport report;
    const Snapshot* s = loaded<Snapshot>(bytes, region, report, snapshotMeasure, snapshotLoad);
    CHECK(s != nullptr && silent(report));
    if (s == nullptr || s->sessions.size() != 2 || s->clients.size() != 2) {
        CHECK(false);
        return;
    }
    CHECK(s->next_session == 3);
    const auto queueAt = [](const StoreSession& v, int32_t i, std::uint8_t* o, int64_t c, int32_t* l, bool* p) {
        return StoreSessionQueueTextAt(v, i, o, c, l, p);
    };
    const auto waitingAt = [](const Driver& v, int32_t i, std::uint8_t* o, int64_t c, int32_t* l, bool* p) {
        return DriverWaitingTextAt(v, i, o, c, l, p);
    };
    for (int32_t n = 0; n < 2; ++n) {
        const StoreSession& x = s->sessions[n];
        const std::uint64_t id = static_cast<std::uint64_t>(n + 1);
        const std::string tag = std::to_string(id);
        CHECK(x.id == id);
        checkInfo(x.info, tag);
        CHECK(x.transcript.size() == 3);
        for (int32_t i = 0; i < x.transcript.size(); ++i) {
            const StoreEntry& e = x.transcript[i];
            CHECK(e.kind == (i == 0 ? EntryKind::Person : EntryKind::Friend));
            CHECK(e.closed == (i != 2));
            CHECK(textOf(e.text) == "entry " + tag + "." + std::to_string(i));
        }
        CHECK(x.queue.size() == 1);
        CHECK(textAt(x, 0, queueAt) == "queued " + tag);
        CHECK(x.driver.next_turn == 10 * id);
        CHECK(x.driver.open_turn == 0);
        CHECK(textOf(x.driver.session) == "s-" + tag);
        CHECK(x.driver.waiting.size() == 1);
        CHECK(textAt(x.driver, 0, waitingAt) == "waiting " + tag);
    }
    for (int32_t i = 0; i < 2; ++i) {
        CHECK(textOf(s->clients[i].client) == "window-" + std::to_string(i + 1));
        CHECK(s->clients[i].applied == static_cast<std::uint64_t>((i + 1) * 1000));
    }
}

// A root with nothing set reads every default: zero, false, absent text, empty lists, and activity Idle (the default
// that claims least, docs/formats.md).
void roundtripDefaults() {
    SessionsDiffBuilder b;
    SessionChange* s = SessionsDiffSessionsAdd(b.main, b.GetRoot()->sessions);
    s->id = 1;
    const Bytes bytes = saved(b, [](const SessionsDiffBuilder& x) { return SessionsDiffMeasure(x); },
        [](const SessionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return SessionsDiffSave(x, out, n); });
    std::vector<std::uint8_t> region;
    TableReport report;
    const SessionsDiff* d = loaded<SessionsDiff>(bytes, region, report, sessionsMeasure, sessionsLoad);
    CHECK(d != nullptr && silent(report));
    if (d != nullptr && d->sessions.size() == 1) {
        const SessionChange& x = d->sessions[0];
        CHECK(x.id == 1 && !x.changed && x.first == 0 && x.keep_from == 0 && !x.queue_changed);
        CHECK(x.activity == Activity::Idle);
        CHECK(absent(x.info.friend_name) && absent(x.info.instance));
        CHECK(x.entries.size() == 0 && x.queue.size() == 0);
    }

    SnapshotBuilder e;
    const Bytes empty = saved(e, [](const SnapshotBuilder& x) { return SnapshotMeasure(x); },
        [](const SnapshotBuilder& x, std::uint8_t* out, int64_t n) { return SnapshotSave(x, out, n); });
    TableReport r2;
    const Snapshot* snap = loaded<Snapshot>(empty, region, r2, snapshotMeasure, snapshotLoad);
    CHECK(snap != nullptr && silent(r2));
    if (snap != nullptr) {
        CHECK(snap->next_session == 0 && snap->sessions.size() == 0 && snap->clients.size() == 0);
    }
}

// Bytes cut short at every length are refused or reported damaged: never read as a smaller value.
void roundtripTruncated() {
    const Bytes whole = recordBytes();
    for (std::size_t n = 0; n < whole.size(); ++n) {
        const Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(n));
        std::vector<std::uint8_t> region;
        TableReport report;
        const Record* r = loaded<Record>(cut, region, report, recordMeasure, recordLoad);
        if (r != nullptr) {
            std::fprintf(stderr, "a record cut to %zu of %zu bytes was read\n", n, whole.size());
        }
        CHECK(r == nullptr);
    }
}

void test_roundtrip() {
    roundtripHello();
    roundtripSessions();
    roundtripActions();
    roundtripRecord();
    roundtripSnapshot();
    roundtripDefaults();
    roundtripTruncated();
}

// ---- versions ----

// This build's side: the same sessions diff and record the later version writes (wire_next.cpp), less what that
// version adds.
Bytes currentSessions() {
    SessionsDiffBuilder b;
    SessionChange* s = SessionsDiffSessionsAdd(b.main, b.GetRoot()->sessions);
    s->id = 7;
    s->changed = true;
    setText(b.main, s->info.friend_name, "rowan");
    setText(b.main, s->info.harness, "claude");
    setText(b.main, s->info.instance, "studio");
    s->activity = Activity::Busy;
    const EntryKind kinds[] = {EntryKind::Person, EntryKind::Tool};
    for (EntryKind k : kinds) {
        EntryChange* e = SessionChangeEntriesAdd(b.main, s->entries);
        e->kind = k;
        e->closed = true;
        setText(b.main, e->tail, k == EntryKind::Person ? "hi" : "ls");
    }
    s->queue_changed = true;
    SessionChangeQueueAppend(b.main, *s, "later");
    return saved(b, [](const SessionsDiffBuilder& x) { return SessionsDiffMeasure(x); },
        [](const SessionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return SessionsDiffSave(x, out, n); });
}

Bytes currentRecord() {
    RecordBuilder b;
    Record* r = b.GetRoot();
    r->session = 7;
    Change* c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::Append;
    c->append = StoreEntry{};
    c->append.kind = EntryKind::Person;
    c->append.closed = true;
    setText(b.main, c->append.text, "hi");
    c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::Enqueue;
    c->enqueue = TableRef{};
    setText(b.main, c->enqueue, "later");
    c = RecordChangesAdd(b.main, r->changes);
    c->type = ChangeType::Dequeue;
    setText(b.main, r->client, "w1");
    r->applied = 3;
    return saved(b, [](const RecordBuilder& x) { return RecordMeasure(x); },
        [](const RecordBuilder& x, std::uint8_t* out, int64_t n) { return RecordSave(x, out, n); });
}

Read currentReadSessions(const Bytes& bytes) {
    Read out;
    std::vector<std::uint8_t> region;
    TableReport report;
    const SessionsDiff* d = loaded<SessionsDiff>(bytes, region, report, sessionsMeasure, sessionsLoad);
    out.loaded = d != nullptr;
    out.unknown = report.unknown;
    out.damaged = report.damaged;
    out.refused = report.refused;
    if (d == nullptr || d->sessions.size() < 1) {
        return out;
    }
    const SessionChange& s = d->sessions[0];
    out.friendName = textOf(s.info.friend_name);
    out.instance = textOf(s.info.instance);
    for (const EntryChange& e : s.entries) {
        out.kinds.push_back(EnumName(e.kind));
    }
    const auto at = [](const SessionChange& v, int32_t i, std::uint8_t* o, int64_t c, int32_t* l, bool* p) {
        return SessionChangeQueueTextAt(v, i, o, c, l, p);
    };
    for (int32_t i = 0; i < s.queue.size(); ++i) {
        out.queue.push_back(textAt(s, i, at));
    }
    return out;
}

Read currentReadRecord(const Bytes& bytes) {
    Read out;
    std::vector<std::uint8_t> region;
    TableReport report;
    const Record* r = loaded<Record>(bytes, region, report, recordMeasure, recordLoad);
    out.loaded = r != nullptr;
    out.unknown = report.unknown;
    out.damaged = report.damaged;
    out.refused = report.refused;
    if (r != nullptr) {
        for (const Change& c : r->changes) {
            out.changes.push_back(EnumName(c.type));
        }
    }
    return out;
}

bool clean(const Read& r) {
    return r.loaded && !r.damaged && !r.refused;
}

void test_versions() {
    using Names = std::vector<std::string>;
    const Bytes oldSessions = currentSessions();
    const Bytes newSessions = wiretest::nextSessions();
    const Bytes oldRecord = currentRecord();
    const Bytes newRecord = wiretest::nextRecord();
    CHECK(!oldSessions.empty() && !newSessions.empty() && !oldRecord.empty() && !newRecord.empty());

    // Each version reads its own bytes with nothing unknown.
    const Read oldOwn = currentReadSessions(oldSessions);
    CHECK(clean(oldOwn) && oldOwn.unknown == 0);
    const Read newOwn = wiretest::nextReadSessions(newSessions);
    CHECK(clean(newOwn) && newOwn.unknown == 0);
    CHECK(newOwn.titlePresent && newOwn.title == "the long one");
    CHECK((newOwn.kinds == Names{"Person", "Image", "Tool"}));

    // An older window reads a newer server's state: the added field is skipped and the added kind reads None (the
    // window shows it as a notice), each counted; everything else is as written.
    const Read oldReadsNew = currentReadSessions(newSessions);
    CHECK(clean(oldReadsNew));
    CHECK(oldReadsNew.unknown == 3);    // the title, the Image kind and the second session's Waiting activity
    CHECK(oldReadsNew.friendName == "rowan");
    CHECK(oldReadsNew.instance == "studio");
    CHECK((oldReadsNew.kinds == Names{"Person", "None", "Tool"}));
    CHECK((oldReadsNew.queue == Names{"later"}));

    // A newer window reads an older server's state: the field the older one never wrote reads its default (absent),
    // and nothing is unknown.
    const Read newReadsOld = wiretest::nextReadSessions(oldSessions);
    CHECK(clean(newReadsOld));
    CHECK(newReadsOld.unknown == 0);
    CHECK(!newReadsOld.titlePresent);
    CHECK(newReadsOld.friendName == "rowan");
    CHECK(newReadsOld.instance == "studio");
    CHECK((newReadsOld.kinds == Names{"Person", "Tool"}));
    CHECK((newReadsOld.queue == Names{"later"}));

    // An older server reads a newer one's store record: the change of the arm it lacks reads None (skipped whole),
    // counted; the rest are as written.
    const Read oldReadsNewRecord = currentReadRecord(newRecord);
    CHECK(clean(oldReadsNewRecord));
    CHECK(oldReadsNewRecord.unknown == 1);
    CHECK((oldReadsNewRecord.changes == Names{"Append", "None", "Enqueue", "Dequeue"}));

    // A newer server reads an older one's store record whole.
    const Read newReadsOldRecord = wiretest::nextReadRecord(oldRecord);
    CHECK(clean(newReadsOldRecord));
    CHECK(newReadsOldRecord.unknown == 0);
    CHECK((newReadsOldRecord.changes == Names{"Append", "Enqueue", "Dequeue"}));
}

struct Group {
    const char* name;
    void (*run)();
};

const Group groups[] = {
    {"roundtrip", test_roundtrip},
    {"versions", test_versions},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : groups) {
        if (argc < 2 || std::strcmp(argv[1], g.name) == 0) {
            g.run();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "wire_test: no group named %s\n", argv[1]);
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "wire_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("wire_test: ok\n");
    return 0;
}
