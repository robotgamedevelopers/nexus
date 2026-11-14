#include "sync/sessions.h"

#include <algorithm>
#include <stdexcept>

#include "sync/wire.h"

namespace nexus::sync {

namespace {

bool sameEntry(const EntryRef& a, const EntryRef& b) {
    return a == b || *a == *b;
}

// EntryKind on the wire: Person 0, Friend 1, Notice 2, Tool 3.
bool validKind(std::uint8_t k) {
    return k <= static_cast<std::uint8_t>(EntryKind::Tool);
}

// Activity on the wire: Idle 0, Busy 1, Down 2.
bool validActivity(std::uint8_t a) {
    return a <= static_cast<std::uint8_t>(Activity::Down);
}

// The entry `s` holds at transcript index i, or null.
const EntryRef* entryAt(const SessionView* s, std::uint64_t i) {
    if (s == nullptr || i < s->first || i >= s->total()) {
        return nullptr;
    }
    return &s->entries[static_cast<std::size_t>(i - s->first)];
}

std::vector<SessionRef>::const_iterator lookup(const std::vector<SessionRef>& sessions, SessionId id) {
    auto it = std::lower_bound(sessions.begin(), sessions.end(), id,
        [](const SessionRef& s, SessionId n) { return s->id < n; });
    return it != sessions.end() && (*it)->id == id ? it : sessions.end();
}

std::string named(SessionId id) {
    return "session " + std::to_string(id);
}

// One changed session: what it is (its friend, harness, account, model and effort); its activity;
// its first index; the index k of
// the first entry `a` does not hold as it is; the entries from k to the end, each its kind, closed flag, the length of
// text `a` holds of it (non-zero only for an entry `a` holds open that only grew) and the rest of its text; then a
// byte, 1 if the queue changed, and if so the queue.
void writeSession(Writer& w, const SessionView& b, const SessionView* a) {
    w.str(b.info.friendName);
    w.str(b.info.harness);
    w.str(b.info.account);
    w.str(b.info.model);
    w.str(b.info.effort);
    w.byte(static_cast<std::uint8_t>(b.activity));
    w.u64(b.first);
    std::uint64_t k = b.first;
    while (k < b.total()) {
        const EntryRef* had = entryAt(a, k);
        if (had == nullptr || !sameEntry(*had, b.entries[static_cast<std::size_t>(k - b.first)])) {
            break;
        }
        ++k;
    }
    w.u64(k);
    w.u64(b.total() - k);
    for (std::uint64_t i = k; i < b.total(); ++i) {
        const SessionEntry& e = *b.entries[static_cast<std::size_t>(i - b.first)];
        const EntryRef* had = entryAt(a, i);
        std::size_t offset = 0;
        if (had != nullptr && !(*had)->closed && (*had)->kind == e.kind && (*had)->text.size() <= e.text.size() &&
                e.text.compare(0, (*had)->text.size(), (*had)->text) == 0) {
            offset = (*had)->text.size();
        }
        w.byte(static_cast<std::uint8_t>(e.kind));
        w.byte(e.closed ? 1 : 0);
        w.u64(offset);
        w.u64(e.text.size() - offset);
        w.raw(e.text.data() + offset, e.text.size() - offset);
    }
    const bool queueChanged = a == nullptr || a->queue != b.queue;
    w.byte(queueChanged ? 1 : 0);
    if (queueChanged) {
        w.u64(b.queue.size());
        for (const std::string& q : b.queue) {
            w.str(q);
        }
    }
}

SessionRef readSession(Reader& r, std::size_t left, SessionId id, const SessionView* a) {
    auto t = std::make_shared<SessionView>();
    t->id = id;
    const std::string name = named(id);
    t->info.friendName = r.str();
    t->info.harness = r.str();
    t->info.account = r.str();
    t->info.model = r.str();
    t->info.effort = r.str();
    const std::uint8_t activity = r.byte();
    if (!validActivity(activity)) {
        throw BadDiff(name + ": activity " + std::to_string(activity) + ", none of idle 0, busy 1 or down 2");
    }
    t->activity = static_cast<Activity>(activity);
    t->first = r.u64();
    const std::uint64_t k = r.u64();
    const std::uint64_t count = r.u64();
    if (k < t->first) {
        throw BadDiff(name + ": diff sends from entry " + std::to_string(k) + ", before its tail's first, " +
            std::to_string(t->first));
    }
    // Entry numbers past 2^64 - 1 would wrap: the session's length, k + count, must be one a u64 holds.
    if (count > ~std::uint64_t(0) - k) {
        throw BadDiff(name + ": diff sends " + std::to_string(count) + " entries from entry " + std::to_string(k) +
            ", past the last entry number there can be");
    }
    // Each entry takes at least four bytes, so a count past what is left of the diff is malformed.
    if (count > left) {
        throw BadDiff(name + ": diff says it sends " + std::to_string(count) + " entries in " + std::to_string(left) +
            " bytes");
    }
    if (k - t->first > (a == nullptr ? 0 : a->entries.size())) {
        throw BadDiff(name + ": diff keeps " + std::to_string(k - t->first) + " entries this state lacks");
    }
    t->entries.reserve(static_cast<std::size_t>(k - t->first + count));
    for (std::uint64_t i = t->first; i < k; ++i) {
        const EntryRef* had = entryAt(a, i);
        if (had == nullptr) {
            throw BadDiff(name + ": diff keeps entry " + std::to_string(i) + ", which this state lacks");
        }
        t->entries.push_back(*had);
    }
    for (std::uint64_t i = k; i < k + count; ++i) {
        const std::uint8_t kind = r.byte();
        if (!validKind(kind)) {
            throw BadDiff(name + ": entry " + std::to_string(i) + " has kind " + std::to_string(kind));
        }
        const std::uint8_t closed = r.byte();
        if (closed > 1) {
            throw BadDiff(name + ": entry " + std::to_string(i) + " has closed flag " + std::to_string(closed));
        }
        const std::uint64_t offset = r.u64();
        std::string text = r.str();
        if (offset != 0) {
            const EntryRef* had = entryAt(a, i);
            if (had == nullptr || (*had)->closed || static_cast<std::uint8_t>((*had)->kind) != kind ||
                    (*had)->text.size() != offset) {
                throw BadDiff(name + ": entry " + std::to_string(i) + " grows from " + std::to_string(offset) +
                    " bytes, which this state does not hold open");
            }
            text = (*had)->text + text;
        }
        t->entries.push_back(std::make_shared<const SessionEntry>(
            SessionEntry{static_cast<EntryKind>(kind), std::move(text), closed == 1}));
    }
    const std::uint8_t queueChanged = r.byte();
    if (queueChanged == 0) {
        if (a == nullptr) {
            throw BadDiff(name + ": diff keeps the queue of a session this state lacks");
        }
        t->queue = a->queue;
    } else if (queueChanged == 1) {
        const std::uint64_t n = r.u64();
        if (n > left) {
            throw BadDiff(name + ": diff says its queue holds " + std::to_string(n) + " prompts in " +
                std::to_string(left) + " bytes");
        }
        for (std::uint64_t q = 0; q < n; ++q) {
            t->queue.push_back(r.str());
        }
    } else {
        throw BadDiff(name + ": queue flag " + std::to_string(queueChanged) + ", neither 0 nor 1");
    }
    return t;
}

} // namespace

bool SessionView::operator==(const SessionView& x) const {
    if (id != x.id || !(info == x.info) || activity != x.activity || first != x.first ||
            entries.size() != x.entries.size() || queue != x.queue) {
        return false;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!sameEntry(entries[i], x.entries[i])) {
            return false;
        }
    }
    return true;
}

SessionView tailOf(SessionId id, SessionInfo info, const std::vector<EntryRef>& transcript,
                   std::vector<std::string> queue, Tail tail, EntryRef open) {
    SessionView t;
    t.id = id;
    t.info = std::move(info);
    t.queue = std::move(queue);
    // The tail of the closed entries, then the open entry, if any, which the tail's rules do not count: so a state
    // always carries the newest closed entry, and a window that missed entries always gets one after its notice.
    const std::size_t count = transcript.size();
    std::size_t from = count;
    std::size_t bytes = 0;
    while (from > 0) {
        const std::size_t size = transcript[from - 1]->text.size();
        const bool newest = from == count;
        if (!newest && (count - from >= tail.entries || bytes + size > tail.bytes)) {
            break;
        }
        bytes += size;
        --from;
    }
    t.first = from;
    for (std::size_t i = from; i < count; ++i) {
        t.entries.push_back(transcript[i]);
    }
    if (open) {
        t.entries.push_back(std::move(open));
    }
    return t;
}

Sessions::Sessions() : sessions_(std::make_shared<const std::vector<SessionRef>>()) {}

Sessions::Sessions(std::vector<SessionRef> sessions) {
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i]->id == 0) {
            throw std::logic_error("Sessions: a session with id 0");
        }
        if (i > 0 && !(sessions[i - 1]->id < sessions[i]->id)) {
            throw std::logic_error("Sessions: sessions out of id order or with one id twice: " +
                std::to_string(sessions[i - 1]->id) + ", " + std::to_string(sessions[i]->id));
        }
    }
    sessions_ = std::make_shared<const std::vector<SessionRef>>(std::move(sessions));
}

const SessionView* Sessions::find(SessionId id) const {
    auto it = lookup(*sessions_, id);
    return it == sessions_->end() ? nullptr : it->get();
}

const SessionView* Sessions::first(const std::string& friendName) const {
    for (const SessionRef& s : *sessions_) {
        if (s->info.friendName == friendName) {
            return s.get();     // in id order: the first found has the lowest id
        }
    }
    return nullptr;
}

// A diff: the number of sessions; then for each, in id order, its id and a byte: 0 when `existing` holds it as it is,
// else 1 and the session (writeSession). A session `existing` holds and this state lacks is not named, which removes
// it.
std::string Sessions::diff_from(const Sessions& existing) const {
    if (*this == existing) {
        return std::string();
    }
    Writer w;
    w.u64(sessions_->size());
    for (const SessionRef& b : *sessions_) {
        w.u64(b->id);
        const SessionView* a = existing.find(b->id);
        if (a != nullptr && (a == b.get() || *a == *b)) {
            w.byte(0);
            continue;
        }
        w.byte(1);
        writeSession(w, *b, a);
    }
    return w.take();
}

std::string Sessions::init_diff() const {
    return diff_from(Sessions());
}

void Sessions::apply_string(const std::string& diff) {
    if (diff.empty()) {
        return;
    }
    Reader r(diff);
    const std::uint64_t n = r.u64();
    if (n > diff.size()) {
        throw BadDiff("sessions diff says it holds " + std::to_string(n) + " sessions in " +
            std::to_string(diff.size()) + " bytes");
    }
    std::vector<SessionRef> sessions;
    sessions.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
        const SessionId id = r.u64();
        if (id == 0) {
            throw BadDiff("sessions diff names session 0; ids start at 1");
        }
        if (!sessions.empty() && !(sessions.back()->id < id)) {
            throw BadDiff("sessions diff names " + named(id) + " out of order or twice");
        }
        const std::uint8_t changed = r.byte();
        auto had = lookup(*sessions_, id);
        const SessionView* a = had == sessions_->end() ? nullptr : had->get();
        if (changed == 0) {
            if (a == nullptr) {
                throw BadDiff("sessions diff keeps " + named(id) + ", which this state lacks");
            }
            sessions.push_back(*had);
        } else if (changed == 1) {
            sessions.push_back(readSession(r, diff.size(), id, a));
        } else {
            throw BadDiff("sessions diff marks " + named(id) + " with " + std::to_string(changed) +
                ", neither 0 nor 1");
        }
    }
    r.end();
    sessions_ = std::make_shared<const std::vector<SessionRef>>(std::move(sessions));
}

bool Sessions::operator==(const Sessions& x) const {
    if (sessions_ == x.sessions_) {
        return true;
    }
    if (sessions_->size() != x.sessions_->size()) {
        return false;
    }
    for (std::size_t i = 0; i < sessions_->size(); ++i) {
        const SessionRef& a = (*sessions_)[i];
        const SessionRef& b = (*x.sessions_)[i];
        if (a != b && !(*a == *b)) {
            return false;
        }
    }
    return true;
}

} // namespace nexus::sync
