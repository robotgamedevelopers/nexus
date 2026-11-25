// The earlier version's side of the cross-version tests (versions.h): tests/wire/prev/NexusPrev.schema's generated code,
// state format 4, writing and reading as a nexus from before the in-app sign-in would. Nothing of this build's version
// is included here.

#include "versions.h"

// MSVC's C4305 off across the generated header only, as src/wire/tables.h does for this version's (it says why).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4305)
#endif
#include "NexusPrevTable.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstring>

namespace wiretest {

namespace {

using namespace nexuswireprev;

void setText(TableWorker& worker, TableRef& slot, const char* text) {
    TableStringEmplace(worker, slot, text, static_cast<int64_t>(std::strlen(text)));
}

std::string textOf(const TableRef& ref) {
    const TableStringView view = TableStringAt(ref);
    return view.data == nullptr ? std::string() : std::string(view.data, static_cast<std::size_t>(view.length));
}

template <typename Builder, typename Measure, typename Save>
Bytes saved(Builder& b, Measure measure, Save save) {
    if (!b.Lock()) {
        return Bytes();
    }
    Bytes out(static_cast<std::size_t>(measure(b)));
    const int64_t wrote = save(b, out.data(), static_cast<int64_t>(out.size()));
    out.resize(wrote < 0 ? 0 : static_cast<std::size_t>(wrote));
    return out;
}

} // namespace

Bytes prevSessions() {
    SessionsDiffBuilder b;
    SessionChange* s = SessionsDiffSessionsAdd(b.main, b.GetRoot()->sessions);
    s->id = 7;
    s->changed = true;
    setText(b.main, s->info.friend_name, "rowan");
    setText(b.main, s->info.harness, "claude");
    setText(b.main, s->info.instance, "studio");
    s->activity = Activity::Busy;
    EntryChange* e = SessionChangeEntriesAdd(b.main, s->entries);
    e->kind = EntryKind::Person;
    e->closed = true;
    setText(b.main, e->tail, "hi");
    s->queue_changed = true;
    SecretRequest* r = SessionsDiffRequestsAdd(b.main, b.GetRoot()->requests);
    r->id = 3;
    setText(b.main, r->account, "a1");
    setText(b.main, r->what, "oauth-token");
    setText(b.main, r->asker_name, "nexus");
    setText(b.main, r->why, "a1 has no login");
    return saved(b, [](const SessionsDiffBuilder& x) { return SessionsDiffMeasure(x); },
        [](const SessionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return SessionsDiffSave(x, out, n); });
}

Bytes prevActions() {
    ActionsDiffBuilder b;
    ActionsDiff* d = b.GetRoot();
    d->start = 5;
    Action* send = ActionsDiffActionsAdd(b.main, d->actions);
    send->kind = ActionKind::Send;
    send->session = 7;
    setText(b.main, send->text, "hi");
    Action* answer = ActionsDiffActionsAdd(b.main, d->actions);
    answer->kind = ActionKind::SecretAnswer;
    answer->request = 3;
    const char value[] = "test-value";
    std::uint8_t* at = TableBytesEmplace(b.main, answer->secret, static_cast<int64_t>(sizeof value - 1));
    if (at != nullptr) {
        std::memcpy(at, value, sizeof value - 1);
    }
    return saved(b, [](const ActionsDiffBuilder& x) { return ActionsDiffMeasure(x); },
        [](const ActionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return ActionsDiffSave(x, out, n); });
}

Bytes prevHello() {
    HelloBuilder b;
    b.GetRoot()->proto = 4;
    b.GetRoot()->min_peer = 1;
    return saved(b, [](const HelloBuilder& x) { return HelloMeasure(x); },
        [](const HelloBuilder& x, std::uint8_t* out, int64_t n) { return HelloSave(x, out, n); });
}

Read prevReadSessions(const Bytes& bytes) {
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
    out.loaded = d != nullptr;
    out.unknown = report.unknown;
    out.damaged = report.damaged;
    out.refused = report.refused;
    if (d == nullptr) {
        return out;
    }
    out.sessions = static_cast<int>(d->sessions.size());
    if (d->sessions.size() >= 1) {
        out.friendName = textOf(d->sessions[0].info.friend_name);
        out.instance = textOf(d->sessions[0].info.instance);
        for (const EntryChange& e : d->sessions[0].entries) {
            out.kinds.push_back(EnumName(e.kind));
            out.texts.push_back(textOf(e.tail));
        }
    }
    for (const SecretRequest& r : d->requests) {
        out.requests.push_back(r.id);
    }
    return out;
}

ReadActions prevReadActions(const Bytes& bytes) {
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

} // namespace wiretest
