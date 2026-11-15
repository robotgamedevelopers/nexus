// The later version's side of the cross-version tests (versions.h): tests/wire/next/NexusNext.schema's generated code,
// writing and reading as a later nexus would. Nothing of this build's version is included here.

#include "versions.h"

// MSVC's C4305 off across the generated header only, as src/wire/tables.h does for this version's (it says why).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4305)
#endif
#include "NexusNextTable.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
    SessionChange* waiting = SessionsDiffSessionsAdd(b.main, b.GetRoot()->sessions);
    waiting->id = 8;
    waiting->changed = true;
    setText(b.main, waiting->info.friend_name, "stella");
    setText(b.main, waiting->info.harness, "claude");
    waiting->activity = Activity::Waiting;
    addEntry(b.main, *waiting, EntryKind::Person, "yo");
    waiting->queue_changed = true;      // a session new to the state sends its queue, here empty
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

Bytes nextActions() {
    ActionsDiffBuilder b;
    ActionsDiff* d = b.GetRoot();
    d->start = 5;
    Action* send = ActionsDiffActionsAdd(b.main, d->actions);
    send->kind = ActionKind::Send;
    send->session = 7;
    setText(b.main, send->text, "hi");
    Action* rename = ActionsDiffActionsAdd(b.main, d->actions);
    rename->kind = ActionKind::Rename;
    rename->session = 7;
    setText(b.main, rename->text, "new name");
    setText(b.main, rename->note, "typed in the tab");
    Action* answer = ActionsDiffActionsAdd(b.main, d->actions);
    answer->kind = ActionKind::SecretAnswer;
    answer->request = 3;
    const char value[] = "test-value";
    std::uint8_t* at = TableBytesEmplace(b.main, answer->secret, static_cast<int64_t>(sizeof value - 1));
    if (at != nullptr) {
        std::memcpy(at, value, sizeof value - 1);
    }
    Action* stop = ActionsDiffActionsAdd(b.main, d->actions);
    stop->kind = ActionKind::Stop;
    stop->session = 7;
    if (!b.Lock()) {
        return Bytes();
    }
    Bytes out(static_cast<std::size_t>(ActionsDiffMeasure(b)));
    const int64_t wrote = ActionsDiffSave(b, out.data(), static_cast<int64_t>(out.size()));
    out.resize(wrote < 0 ? 0 : static_cast<std::size_t>(wrote));
    return out;
}

ReadActions nextReadActions(const Bytes& bytes) {
    ReadActions out;
    const int64_t need = ActionsDiffLoadMeasure(bytes.data(), static_cast<int64_t>(bytes.size()));
    if (need < 0) {
        out.refused = true;
        return out;
    }
    std::vector<std::uint8_t> region(static_cast<std::size_t>(need));
    TableReport report;
    const ActionsDiff* d = ActionsDiffLoad(region.data(), need, bytes.data(), static_cast<int64_t>(bytes.size()),
        &report);
    out.loaded = d != nullptr;
    out.unknown = report.unknown;
    out.damaged = report.damaged;
    out.refused = report.refused;
    if (d == nullptr) {
        return out;
    }
    out.start = d->start;
    for (const Action& a : d->actions) {
        out.kinds.push_back(EnumName(a.kind));
        out.sessions.push_back(a.session);
        out.friends.push_back(textOf(a.friend_name));
        out.texts.push_back(textOf(a.text));
        out.requests.push_back(a.request);
        const TableBytesView secret = TableBytesAt(a.secret);
        out.secrets.push_back(secret.data == nullptr ? std::string() :
            std::string(reinterpret_cast<const char*>(secret.data), static_cast<std::size_t>(secret.length)));
    }
    return out;
}

Bytes nextHello(std::uint32_t proto, std::uint32_t minPeer) {
    HelloBuilder b;
    b.GetRoot()->proto = proto;
    b.GetRoot()->min_peer = minPeer;
    setText(b.main, b.GetRoot()->build, "next-build");
    if (!b.Lock()) {
        return Bytes();
    }
    Bytes out(static_cast<std::size_t>(HelloMeasure(b)));
    const int64_t wrote = HelloSave(b, out.data(), static_cast<int64_t>(out.size()));
    out.resize(wrote < 0 ? 0 : static_cast<std::size_t>(wrote));
    return out;
}

ReadHello nextReadHello(const Bytes& bytes) {
    ReadHello out;
    const int64_t need = HelloLoadMeasure(bytes.data(), static_cast<int64_t>(bytes.size()));
    if (need < 0) {
        return out;
    }
    std::vector<std::uint8_t> region(static_cast<std::size_t>(need));
    TableReport report;
    const Hello* h = HelloLoad(region.data(), need, bytes.data(), static_cast<int64_t>(bytes.size()), &report);
    out.loaded = h != nullptr && !report.damaged && !report.refused;
    out.unknown = report.unknown;
    if (h != nullptr) {
        out.proto = h->proto;
        out.minPeer = h->min_peer;
        out.buildPresent = TableStringAt(h->build).data != nullptr;
    }
    return out;
}

} // namespace wiretest
