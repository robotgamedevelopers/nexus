// The later version's side of the cross-version tests (versions.h): tests/wire/next/NexusNext.schema's generated code,
// writing and reading as a later nexus would. Nothing of this build's version is included here.

#include "versions.h"

#include "NexusNextTable.h"

#include <cstring>

namespace wiretest {

namespace {

using namespace nexuswirenext;

void setText(TableWorker& worker, TableRef& slot, const char* text) {
    TableStringEmplace(worker, slot, text, static_cast<int64_t>(std::strlen(text)));
}

std::string textOf(const TableRef& ref) {
    const TableStringView view = TableStringAt(ref);
    return view.data == nullptr ? std::string() : std::string(view.data, static_cast<std::size_t>(view.length));
}

void addEntry(TableWorker& worker, SessionChange& session, EntryKind kind, const char* text) {
    EntryChange* e = SessionChangeEntriesAdd(worker, session.entries);
    e->kind = kind;
    e->closed = true;
    setText(worker, e->tail, text);
}

void reportInto(Read& out, const TableReport& report, bool loaded) {
    out.loaded = loaded;
    out.unknown = report.unknown;
    out.damaged = report.damaged;
    out.refused = report.refused;
}

} // namespace

Bytes nextSessions() {
    SessionsDiffBuilder b;
    SessionChange* s = SessionsDiffSessionsAdd(b.main, b.GetRoot()->sessions);
    s->id = 7;
    s->changed = true;
    setText(b.main, s->info.friend_name, "rowan");
    setText(b.main, s->info.harness, "claude");
    setText(b.main, s->info.instance, "studio");
    setText(b.main, s->info.title, "the long one");
    s->activity = Activity::Busy;
    addEntry(b.main, *s, EntryKind::Person, "hi");
    addEntry(b.main, *s, EntryKind::Image, "a picture");
    addEntry(b.main, *s, EntryKind::Tool, "ls");
    s->queue_changed = true;
    SessionChangeQueueAppend(b.main, *s, "later");
    if (!b.Lock()) {
        return Bytes();
    }
    Bytes out(static_cast<std::size_t>(SessionsDiffMeasure(b)));
    const int64_t wrote = SessionsDiffSave(b, out.data(), static_cast<int64_t>(out.size()));
    out.resize(wrote < 0 ? 0 : static_cast<std::size_t>(wrote));
    return out;
}

Bytes nextRecord() {
    RecordBuilder b;
    Record* r = b.GetRoot();
    r->session = 7;
    Change* append = RecordChangesAdd(b.main, r->changes);
    append->type = ChangeType::Append;
    append->append = StoreEntry{};
    append->append.kind = EntryKind::Person;
    append->append.closed = true;
    setText(b.main, append->append.text, "hi");
    Change* title = RecordChangesAdd(b.main, r->changes);
    title->type = ChangeType::SetTitle;
    title->set_title = TableRef{};
    setText(b.main, title->set_title, "renamed");
    Change* enqueue = RecordChangesAdd(b.main, r->changes);
    enqueue->type = ChangeType::Enqueue;
    enqueue->enqueue = TableRef{};
    setText(b.main, enqueue->enqueue, "later");
    Change* dequeue = RecordChangesAdd(b.main, r->changes);
    dequeue->type = ChangeType::Dequeue;
    setText(b.main, r->client, "w1");
    r->applied = 3;
    if (!b.Lock()) {
        return Bytes();
    }
    Bytes out(static_cast<std::size_t>(RecordMeasure(b)));
    const int64_t wrote = RecordSave(b, out.data(), static_cast<int64_t>(out.size()));
    out.resize(wrote < 0 ? 0 : static_cast<std::size_t>(wrote));
    return out;
}

Read nextReadSessions(const Bytes& bytes) {
    Read out;
    const int64_t need = SessionsDiffLoadMeasure(bytes.data(), static_cast<int64_t>(bytes.size()));
    if (need < 0) {
        out.refused = true;
        return out;
    }
    std::vector<std::uint8_t> region(static_cast<std::size_t>(need));
    TableReport report;
    const SessionsDiff* d = SessionsDiffLoad(region.data(), need, bytes.data(), static_cast<int64_t>(bytes.size()),
        &report);
    reportInto(out, report, d != nullptr);
    if (d == nullptr || d->sessions.size() < 1) {
        return out;
    }
    const SessionChange& s = d->sessions[0];
    out.friendName = textOf(s.info.friend_name);
    out.instance = textOf(s.info.instance);
    out.titlePresent = TableStringAt(s.info.title).data != nullptr;
    out.title = textOf(s.info.title);
    for (const EntryChange& e : s.entries) {
        out.kinds.push_back(EnumName(e.kind));
    }
    for (int32_t i = 0; i < s.queue.size(); ++i) {
        int32_t length = 0;
        SessionChangeQueueTextAt(s, i, nullptr, 0, &length, nullptr);
        std::string text(static_cast<std::size_t>(length), '\0');
        SessionChangeQueueTextAt(s, i, reinterpret_cast<std::uint8_t*>(text.data()), length, &length, nullptr);
        out.queue.push_back(text);
    }
    return out;
}

Read nextReadRecord(const Bytes& bytes) {
    Read out;
    const int64_t need = RecordLoadMeasure(bytes.data(), static_cast<int64_t>(bytes.size()));
    if (need < 0) {
        out.refused = true;
        return out;
    }
    std::vector<std::uint8_t> region(static_cast<std::size_t>(need));
    TableReport report;
    const Record* r = RecordLoad(region.data(), need, bytes.data(), static_cast<int64_t>(bytes.size()), &report);
    reportInto(out, report, r != nullptr);
    if (r == nullptr) {
        return out;
    }
    for (const Change& c : r->changes) {
        out.changes.push_back(EnumName(c.type));
    }
    return out;
}

} // namespace wiretest
