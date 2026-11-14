#include "sync/sessions.h"

#include <algorithm>
#include <stdexcept>

#include "sync/wire.h"

namespace nexus::sync {

namespace {

bool sameEntry(const EntryRef& a, const EntryRef& b) {
    return a == b || *a == *b;
}

// EntryKind on the wire: Person 0, Friend 1, Notice 2, Tool 3 (from format 2). Format 1 has no Tool: a tool line goes
// to a window of format 1 as the friend's text, as servers before format 2 sent it. A kind this build does not know (a
// later server's) is read as a notice, which the window draws as a plain dim line.
std::uint8_t kindOnWire(EntryKind k, std::uint32_t format) {
    if (format < 2 && k == EntryKind::Tool) {
        return static_cast<std::uint8_t>(EntryKind::Friend);
    }
    return static_cast<std::uint8_t>(k);
}

EntryKind kindRead(std::uint8_t k) {
    return k <= static_cast<std::uint8_t>(EntryKind::Tool) ? static_cast<EntryKind>(k) : EntryKind::Notice;
}

// Activity on the wire, from format 2: Idle 0, Busy 1, Down 2. Format 1 carries none, and a session of it reads idle:
// that server cannot say, and idle claims least (the person may send, which is always safe, where busy would have them
// wait on nothing). A value this build does not know (a later server's) reads busy: the server said the friend is
// neither idle nor down, so not waiting on the person.
Activity activityRead(std::uint8_t a) {
    return a <= static_cast<std::uint8_t>(Activity::Down) ? static_cast<Activity>(a) : Activity::Busy;
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

// One entry: its kind, closed flag, the length of text `a` holds of it (non-zero only for an entry `a` holds open that
// only grew) and the rest of its text. From format 2 these are framed by their length, and fields a later format adds
// go after them.
void writeEntry(Writer& w, const SessionEntry& e, const EntryRef* had, std::uint32_t format) {
    std::size_t offset = 0;
    if (had != nullptr && !(*had)->closed && (*had)->kind == e.kind && (*had)->text.size() <= e.text.size() &&
            e.text.compare(0, (*had)->text.size(), (*had)->text) == 0) {
        offset = (*had)->text.size();
    }
    Writer body;
    Writer& out = format < 2 ? w : body;
    out.byte(kindOnWire(e.kind, format));
    out.byte(e.closed ? 1 : 0);
    out.u64(offset);
    out.u64(e.text.size() - offset);
    out.raw(e.text.data() + offset, e.text.size() - offset);
    if (format >= 2) {
        w.str(body.take());
    }
}

// One changed session: what it is (its friend, harness, account, model and effort); from format 2 its activity; its
// first index; the index k of the first entry `a` does not hold as it is; the entries from k to the end; then a byte, 1
// if the queue changed, and if so the queue. From format 2 the whole is framed by its length, and fields a later format
// adds go after the queue.
void writeSession(Writer& w, const SessionView& b, const SessionView* a, std::uint32_t format) {
    Writer body;
    Writer& out = format < 2 ? w : body;
    out.str(b.info.friendName);
    out.str(b.info.harness);
    out.str(b.info.account);
    out.str(b.info.model);
    out.str(b.info.effort);
    if (format >= 2) {
        out.byte(static_cast<std::uint8_t>(b.activity));
    }
    out.u64(b.first);
    std::uint64_t k = b.first;
    while (k < b.total()) {
        const EntryRef* had = entryAt(a, k);
        if (had == nullptr || !sameEntry(*had, b.entries[static_cast<std::size_t>(k - b.first)])) {
            break;
        }
        ++k;
    }
    out.u64(k);
    out.u64(b.total() - k);
    for (std::uint64_t i = k; i < b.total(); ++i) {
        writeEntry(out, *b.entries[static_cast<std::size_t>(i - b.first)], entryAt(a, i), format);
    }
    const bool queueChanged = a == nullptr || a->queue != b.queue;
    out.byte(queueChanged ? 1 : 0);
    if (queueChanged) {
        out.u64(b.queue.size());
        for (const std::string& q : b.queue) {
            out.str(q);
        }
    }
    if (format >= 2) {
        w.str(body.take());
    }
}

// Entry i, read from `r` (format 1) or from its frame (format 2 on, whose fields past the text are skipped).
EntryRef readEntry(Reader& r, const std::string& name, std::uint64_t i, const SessionView* a, std::uint32_t format) {
    std::string frame;
    if (format >= 2) {
        frame = r.str();
    }
    Reader framed(frame);
    Reader& in = format < 2 ? r : framed;
    const EntryKind kind = kindRead(in.byte());
    const std::uint8_t closed = in.byte();
    if (closed > 1) {
        throw BadDiff(name + ": entry " + std::to_string(i) + " has closed flag " + std::to_string(closed));
    }
    const std::uint64_t offset = in.u64();
    std::string text = in.str();
    if (offset != 0) {
        const EntryRef* had = entryAt(a, i);
        if (had == nullptr || (*had)->closed || (*had)->kind != kind || (*had)->text.size() != offset) {
            throw BadDiff(name + ": entry " + std::to_string(i) + " grows from " + std::to_string(offset) +
                " bytes, which this state does not hold open");
        }
        text = (*had)->text + text;
    }
    return std::make_shared<const SessionEntry>(SessionEntry{kind, std::move(text), closed == 1});
}

// A changed session's fields, from `r`; `left` bounds the counts it may declare (the bytes there are to read).
SessionRef readFields(Reader& r, std::size_t left, SessionId id, const SessionView* a, std::uint32_t format) {
    auto t = std::make_shared<SessionView>();
    t->id = id;
    const std::string name = named(id);
    t->info.friendName = r.str();
    t->info.harness = r.str();
    t->info.account = r.str();
    t->info.model = r.str();
    t->info.effort = r.str();
    t->activity = format >= 2 ? activityRead(r.byte()) : Activity::Idle;
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
    // Each entry takes at least one byte, so a count past what is left of the diff is malformed.
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
        t->entries.push_back(readEntry(r, name, i, a, format));
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

// A changed session: in format 1 read straight from the diff; from format 2 from its frame, whose fields past the
// queue (a later format's) are skipped.
SessionRef readSession(Reader& r, std::size_t left, SessionId id, const SessionView* a, std::uint32_t format) {
    if (format < 2) {
        return readFields(r, left, id, a, format);
    }
    const std::string frame = r.str();
    Reader framed(frame);
    return readFields(framed, frame.size(), id, a, format);
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
// else 1 and the session (writeSession), in this state's format. A session `existing` holds and this state lacks is
// not named, which removes it.
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
        writeSession(w, *b, a, format_);
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
            sessions.push_back(readSession(r, diff.size(), id, a, format_));
        } else {
            throw BadDiff("sessions diff marks " + named(id) + " with " + std::to_string(changed) +
                ", neither 0 nor 1");
        }
    }
    // Format 1 ends with its sessions; from format 2 what a later format adds after them is skipped.
    if (format_ < 2) {
        r.end();
    }
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

namespace {
constexpr const char* kStateFileTag = "nexus-state ";
}

std::string stateFile(const Sessions& state) {
    return kStateFileTag + std::to_string(state.format()) + "\n" + state.init_diff();
}

bool readStateFile(const std::string& bytes, Sessions* out, std::string* why) {
    const std::size_t tag = std::char_traits<char>::length(kStateFileTag);
    const std::size_t end = bytes.find('\n');
    std::uint32_t format = 0;
    bool number = bytes.compare(0, tag, kStateFileTag) == 0 && end != std::string::npos && end > tag && end - tag < 10;
    for (std::size_t i = tag; number && i < end; ++i) {
        number = bytes[i] >= '0' && bytes[i] <= '9';
        format = format * 10 + std::uint32_t(bytes[i] - '0');
    }
    if (!number || format == 0) {
        *why = "not a state file: it does not start with a line \"nexus-state <format>\"";
        return false;
    }
    if (format < kOldestServerFormat) {
        *why = "a state of format " + std::to_string(format) + ", older than this build reads (" +
            std::to_string(kOldestServerFormat) + " on)";
        return false;
    }
    Sessions state;
    state.setFormat(linkFormat(format, kStateFormat));
    try {
        state.apply_string(bytes.substr(end + 1));
    } catch (const BadDiff& e) {
        *why = std::string("not a state of format ") + std::to_string(format) + ": " + e.what();
        return false;
    }
    *out = state;
    return true;
}

} // namespace nexus::sync
