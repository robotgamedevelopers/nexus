// Format 4 on (sync/formats.h, docs/formats.md): the state and the actions as schema's variable tables, SessionsDiff and
// ActionsDiff of schema/Nexus.schema, through the generated code (schema/gen, src/wire/tables.h). Nothing here encodes
// a byte by hand: this file copies between nexus's own types and the generated ones.
//
// What crosses versions, by the tables' own rules: a field rides under the hash of its name, so a reader skips one it
// does not know (counted in report.unknown) and reads one an older writer left out as its declared default; an enum
// value it does not know reads None, counted too. A read refuses only bytes that do not load (damaged, cut short, or a
// form this build does not carry).
//
// Text: a table's text is well-formed UTF-8 with no zero byte, and a reader refuses the whole diff otherwise. nexus's
// strings are bytes, so text that breaks the rule is written with each offending byte as U+FFFD (tableText).

#include "sync/codec.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

#include "wire/tables.h"

namespace nexus::sync::codec {

namespace {

namespace wire = nexuswire;

// The length of the UTF-8 sequence at text[i] when it is well formed and not a zero byte, else 0 (the rules of
// TableUtf8Valid: no overlong form, no surrogate, nothing past U+10FFFF).
std::size_t sequenceAt(std::string_view text, std::size_t i) {
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = at(i);
    if (lead == 0) {
        return 0;
    }
    if (lead < 0x80) {
        return 1;
    }
    std::size_t n = 0;
    std::uint32_t cp = 0;
    std::uint32_t least = 0;
    if ((lead & 0xE0) == 0xC0) {
        n = 2;
        cp = lead & 0x1Fu;
        least = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        n = 3;
        cp = lead & 0x0Fu;
        least = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        n = 4;
        cp = lead & 0x07u;
        least = 0x10000;
    } else {
        return 0;
    }
    if (text.size() - i < n) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        const unsigned char c = at(i + k);
        if ((c & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

// `text` as a table's text: as it is when it already is one, else with each byte that starts no well-formed sequence
// (or is zero) replaced by U+FFFD.
std::string_view asText(std::string_view text, std::string& scratch) {
    if (tableText(text)) {
        return text;
    }
    scratch.clear();
    scratch.reserve(text.size() + 8);
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t n = sequenceAt(text, i);
        if (n == 0) {
            scratch += "\xEF\xBF\xBD";
            ++i;
        } else {
            scratch.append(text.data() + i, n);
            i += n;
        }
    }
    return scratch;
}

// A *string field: absent when empty (which reads as empty), else the text.
void putText(wire::TableWorker& worker, wire::TableRef& slot, std::string_view text, std::string& scratch) {
    if (text.empty()) {
        return;
    }
    const std::string_view t = asText(text, scratch);
    if (wire::TableStringEmplace(worker, slot, t.data(), static_cast<int64_t>(t.size())) == nullptr) {
        throw std::bad_alloc();
    }
}

std::string textOf(const wire::TableRef& ref) {
    const wire::TableStringView v = wire::TableStringAt(ref);
    return v.data == nullptr ? std::string() : std::string(v.data, static_cast<std::size_t>(v.length));
}

template <typename T>
T* added(T* node) {
    if (node == nullptr) {
        throw std::bad_alloc();
    }
    return node;
}

wire::EntryKind kindOut(EntryKind k) {
    switch (k) {
        case EntryKind::Person: return wire::EntryKind::Person;
        case EntryKind::Friend: return wire::EntryKind::Friend;
        case EntryKind::Notice: return wire::EntryKind::Notice;
        case EntryKind::Tool: return wire::EntryKind::Tool;
    }
    return wire::EntryKind::Notice;
}

// An entry kind this build does not know (None: a later server's, or one an older writer did not set) is a notice, a
// plain dim line with the entry's text: the text is never lost, and a notice claims nothing about who wrote it.
EntryKind kindIn(wire::EntryKind k) {
    switch (k) {
        case wire::EntryKind::Person: return EntryKind::Person;
        case wire::EntryKind::Friend: return EntryKind::Friend;
        case wire::EntryKind::Notice: return EntryKind::Notice;
        case wire::EntryKind::Tool: return EntryKind::Tool;
        default: return EntryKind::Notice;
    }
}

wire::Activity activityOut(Activity a) {
    switch (a) {
        case Activity::Idle: return wire::Activity::Idle;
        case Activity::Busy: return wire::Activity::Busy;
        case Activity::Down: return wire::Activity::Down;
    }
    return wire::Activity::Busy;
}

// Absent reads its declared default, Idle (schema/Nexus.schema); a value this build does not know (None) reads busy:
// the server said the friend is neither idle nor down.
Activity activityIn(wire::Activity a) {
    switch (a) {
        case wire::Activity::Idle: return Activity::Idle;
        case wire::Activity::Down: return Activity::Down;
        default: return Activity::Busy;
    }
}

wire::ActionKind actionOut(ActionKind k) {
    switch (k) {
        case ActionKind::Unknown: return wire::ActionKind::None;
        case ActionKind::Send: return wire::ActionKind::Send;
        case ActionKind::Queue: return wire::ActionKind::Queue;
        case ActionKind::Stop: return wire::ActionKind::Stop;
        case ActionKind::Create: return wire::ActionKind::Create;
        case ActionKind::Ensure: return wire::ActionKind::Ensure;
        case ActionKind::SetAccount: return wire::ActionKind::SetAccount;
        case ActionKind::SetModel: return wire::ActionKind::SetModel;
        case ActionKind::SetEffort: return wire::ActionKind::SetEffort;
        case ActionKind::AddAccount: return wire::ActionKind::AddAccount;
        case ActionKind::RemoveAccount: return wire::ActionKind::RemoveAccount;
        case ActionKind::AddFriend: return wire::ActionKind::AddFriend;
        case ActionKind::LinkAccount: return wire::ActionKind::LinkAccount;
        case ActionKind::UnlinkAccount: return wire::ActionKind::UnlinkAccount;
        case ActionKind::SetDefault: return wire::ActionKind::SetDefault;
        case ActionKind::SecretAnswer: return wire::ActionKind::SecretAnswer;
        case ActionKind::SecretCancel: return wire::ActionKind::SecretCancel;
    }
    return wire::ActionKind::None;
}

// A kind this build does not know (None: a later window's) is Unknown, which the server counts and never applies.
ActionKind actionIn(wire::ActionKind k) {
    switch (k) {
        case wire::ActionKind::Send: return ActionKind::Send;
        case wire::ActionKind::Queue: return ActionKind::Queue;
        case wire::ActionKind::Stop: return ActionKind::Stop;
        case wire::ActionKind::Create: return ActionKind::Create;
        case wire::ActionKind::Ensure: return ActionKind::Ensure;
        case wire::ActionKind::SetAccount: return ActionKind::SetAccount;
        case wire::ActionKind::SetModel: return ActionKind::SetModel;
        case wire::ActionKind::SetEffort: return ActionKind::SetEffort;
        case wire::ActionKind::AddAccount: return ActionKind::AddAccount;
        case wire::ActionKind::RemoveAccount: return ActionKind::RemoveAccount;
        case wire::ActionKind::AddFriend: return ActionKind::AddFriend;
        case wire::ActionKind::LinkAccount: return ActionKind::LinkAccount;
        case wire::ActionKind::UnlinkAccount: return ActionKind::UnlinkAccount;
        case wire::ActionKind::SetDefault: return ActionKind::SetDefault;
        case wire::ActionKind::SecretAnswer: return ActionKind::SecretAnswer;
        case wire::ActionKind::SecretCancel: return ActionKind::SecretCancel;
        default: return ActionKind::Unknown;
    }
}

// The bytes of a builder: measured, then saved into a string of exactly that size.
template <typename Builder, typename Measure, typename Save>
std::string saved(const Builder& b, Measure measure, Save save, const char* what) {
    const int64_t need = measure(b);
    if (need <= 0) {
        throw std::logic_error(std::string(what) + ": schema's measure failed (" + std::to_string(need) + ")");
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    const int64_t wrote = save(b, reinterpret_cast<std::uint8_t*>(out.data()), need);
    if (wrote != need) {
        throw std::logic_error(std::string(what) + ": schema's save wrote " + std::to_string(wrote) + " of " +
            std::to_string(need) + " bytes");
    }
    return out;
}

// A builder's memory, every byte overwritten when it is freed: for an actions diff that holds a secret's value, which
// the builder copied into its arena (sync/secrets.h: where a value may be). Each block keeps its size in front of it.
constexpr std::size_t kWipeHeader = 16;

void* wipingAlloc(void*, int64_t bytes) {
    if (bytes <= 0) {
        return nullptr;
    }
    const std::size_t size = static_cast<std::size_t>(bytes);
    auto* block = static_cast<unsigned char*>(std::calloc(1, size + kWipeHeader));
    if (block == nullptr) {
        return nullptr;
    }
    std::memcpy(block, &size, sizeof size);
    return block + kWipeHeader;
}

void wipingFree(void*, void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    unsigned char* block = static_cast<unsigned char*>(pointer) - kWipeHeader;
    std::size_t size = 0;
    std::memcpy(&size, block, sizeof size);
    volatile unsigned char* bytes = block;
    for (std::size_t i = 0; i < size + kWipeHeader; ++i) {
        bytes[i] = 0;
    }
    std::free(block);
}

// The region a load reads into, wiped when it goes: it may hold a secret's value.
struct Region {
    std::vector<std::uint8_t> bytes;
    ~Region() {
        volatile std::uint8_t* p = bytes.data();
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            p[i] = 0;
        }
    }
};

} // namespace

bool tableText(std::string_view text) {
    return wire::TableUtf8Valid(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::string writeTables(const SessionsChange& change) {
    wire::SessionsDiffBuilder b;
    wire::SessionsDiff* d = b.GetRoot();
    if (d == nullptr) {
        throw std::bad_alloc();
    }
    std::string scratch;
    // Each element is filled whole before the next is added to its list.
    for (const SessionChange& s : change.sessions) {
        wire::SessionChange* x = added(wire::SessionsDiffSessionsAdd(b.main, d->sessions));
        x->id = s.id;
        if (!s.changed) {
            continue;
        }
        x->changed = true;
        putText(b.main, x->info.friend_name, s.info.friendName, scratch);
        putText(b.main, x->info.harness, s.info.harness, scratch);
        putText(b.main, x->info.account, s.info.account, scratch);
        putText(b.main, x->info.model, s.info.model, scratch);
        putText(b.main, x->info.effort, s.info.effort, scratch);
        putText(b.main, x->info.instance, s.info.instance, scratch);
        x->activity = activityOut(s.activity);
        x->first = s.first;
        x->keep_from = s.keepFrom;
        for (const EntryChange& e : s.entries) {
            wire::EntryChange* y = added(wire::SessionChangeEntriesAdd(b.main, x->entries));
            y->kind = kindOut(e.kind);
            y->closed = e.closed;
            y->offset = e.offset;
            putText(b.main, y->tail, e.tail, scratch);
        }
        x->queue_changed = s.queueChanged;
        for (const std::string& q : s.queue) {
            const std::string_view t = asText(q, scratch);
            const std::string owned(t);     // the generated append takes a zero-terminated string
            if (!wire::SessionChangeQueueAppend(b.main, *x, owned.c_str())) {
                throw std::bad_alloc();
            }
        }
    }
    for (const SecretRequest& q : change.requests) {
        wire::SecretRequest* r = added(wire::SessionsDiffRequestsAdd(b.main, d->requests));
        r->id = q.id;
        putText(b.main, r->account, q.account, scratch);
        putText(b.main, r->what, q.what, scratch);
        r->asker = q.asker;
        putText(b.main, r->asker_name, q.askerName, scratch);
        putText(b.main, r->why, q.why, scratch);
    }
    return saved(b, [](const wire::SessionsDiffBuilder& x) { return wire::SessionsDiffMeasure(x); },
        [](const wire::SessionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return wire::SessionsDiffSave(x, out, n); },
        "sessions diff");
}

SessionsChange readTables(const std::string& diff, int* unknown) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(diff.data());
    const int64_t size = static_cast<int64_t>(diff.size());
    const int64_t need = wire::SessionsDiffLoadMeasure(bytes, size);
    if (need < 0) {
        throw BadDiff("sessions diff of " + std::to_string(diff.size()) + " bytes does not load as schema's "
            "SessionsDiff (damaged, cut short, or a form this build does not read)");
    }
    Region region;
    region.bytes.resize(static_cast<std::size_t>(need));
    wire::TableReport report;
    const wire::SessionsDiff* d = wire::SessionsDiffLoad(region.bytes.data(), need, bytes, size, &report);
    if (d == nullptr || report.damaged || report.refused) {
        throw BadDiff("sessions diff of " + std::to_string(diff.size()) + " bytes does not load as schema's "
            "SessionsDiff (damaged, cut short, or a form this build does not read)");
    }
    *unknown = report.unknown;
    SessionsChange out;
    out.sessions.reserve(static_cast<std::size_t>(d->sessions.size()));
    for (const wire::SessionChange& x : d->sessions) {
        SessionChange s;
        s.id = x.id;
        s.changed = x.changed;
        if (s.changed) {
            s.info.friendName = textOf(x.info.friend_name);
            s.info.harness = textOf(x.info.harness);
            s.info.account = textOf(x.info.account);
            s.info.model = textOf(x.info.model);
            s.info.effort = textOf(x.info.effort);
            s.info.instance = textOf(x.info.instance);
            s.activity = activityIn(x.activity);
            s.first = x.first;
            s.keepFrom = x.keep_from;
            s.entries.reserve(static_cast<std::size_t>(x.entries.size()));
            for (const wire::EntryChange& y : x.entries) {
                s.entries.push_back(EntryChange{kindIn(y.kind), y.closed, y.offset, textOf(y.tail)});
            }
            s.queueChanged = x.queue_changed;
            for (const wire::SessionChangeQueueText& q : x.queue) {
                s.queue.push_back(textOf(q.value));
            }
        }
        out.sessions.push_back(std::move(s));
    }
    for (const wire::SecretRequest& r : d->requests) {
        SecretRequest q;
        q.id = r.id;
        q.account = textOf(r.account);
        q.what = textOf(r.what);
        q.asker = r.asker;
        q.askerName = textOf(r.asker_name);
        q.why = textOf(r.why);
        out.requests.push_back(std::move(q));
    }
    return out;
}

std::string writeActionsTables(std::uint64_t start, const std::vector<Action>& held, std::size_t from) {
    bool secret = false;
    for (std::size_t i = from; i < held.size(); ++i) {
        secret = secret || !held[i].secret.empty();
    }
    const wire::TableAllocator wiping{wipingAlloc, wipingFree, nullptr};
    wire::ActionsDiffBuilder b(secret ? wiping : wire::TableDefaultAllocator());
    wire::ActionsDiff* d = b.GetRoot();
    if (d == nullptr) {
        throw std::bad_alloc();
    }
    d->start = start;
    std::string scratch;
    for (std::size_t i = from; i < held.size(); ++i) {
        const Action& a = held[i];
        wire::Action* x = added(wire::ActionsDiffActionsAdd(b.main, d->actions));
        x->kind = actionOut(a.kind);
        x->session = a.session;
        putText(b.main, x->friend_name, a.friendName, scratch);
        putText(b.main, x->text, a.text, scratch);
        putText(b.main, x->account, a.account, scratch);
        x->request = a.request;
        if (!a.secret.empty()) {
            const std::string& value = a.secret.bytes();
            std::uint8_t* to = wire::TableBytesEmplace(b.main, x->secret, static_cast<int64_t>(value.size()));
            if (to == nullptr) {
                throw std::bad_alloc();
            }
            std::memcpy(to, value.data(), value.size());
        }
    }
    return saved(b, [](const wire::ActionsDiffBuilder& x) { return wire::ActionsDiffMeasure(x); },
        [](const wire::ActionsDiffBuilder& x, std::uint8_t* to, int64_t n) { return wire::ActionsDiffSave(x, to, n); },
        "actions diff");
}

std::vector<Action> readActionsTables(const std::string& diff, int* unknown) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(diff.data());
    const int64_t size = static_cast<int64_t>(diff.size());
    const int64_t need = wire::ActionsDiffLoadMeasure(bytes, size);
    if (need < 0) {
        throw BadDiff("actions diff of " + std::to_string(diff.size()) + " bytes does not load as schema's "
            "ActionsDiff (damaged, cut short, or a form this build does not read)");
    }
    Region region;
    region.bytes.resize(static_cast<std::size_t>(need));
    wire::TableReport report;
    const wire::ActionsDiff* d = wire::ActionsDiffLoad(region.bytes.data(), need, bytes, size, &report);
    if (d == nullptr || report.damaged || report.refused) {
        throw BadDiff("actions diff of " + std::to_string(diff.size()) + " bytes does not load as schema's "
            "ActionsDiff (damaged, cut short, or a form this build does not read)");
    }
    *unknown = report.unknown;
    const std::uint64_t count = static_cast<std::uint64_t>(d->actions.size());
    if (d->start == 0) {
        throw BadDiff("actions diff starts at number 0; numbers start at 1");
    }
    if (count == 0) {
        throw BadDiff("actions diff holds no actions");
    }
    if (d->start + count < d->start) {
        throw BadDiff("actions diff numbers run past 64 bits");
    }
    std::vector<Action> out;
    out.reserve(static_cast<std::size_t>(count));
    std::uint64_t num = d->start;
    for (const wire::Action& x : d->actions) {
        Action a;
        a.num = num++;
        a.kind = actionIn(x.kind);
        if (a.kind != ActionKind::Unknown) {
            a.session = x.session;
            a.friendName = textOf(x.friend_name);
            a.text = textOf(x.text);
            a.account = textOf(x.account);
            a.request = x.request;
            const wire::TableBytesView value = wire::TableBytesAt(x.secret);
            if (value.data != nullptr && value.length > 0) {
                a.secret = SecretValue(std::string_view(reinterpret_cast<const char*>(value.data),
                    static_cast<std::size_t>(value.length)));
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

} // namespace nexus::sync::codec
