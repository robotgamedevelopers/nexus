#include "sync/sessions.h"

#include <algorithm>
#include <stdexcept>

#include "sync/codec.h"
#include "sync/wire.h"

namespace nexus::sync {

namespace {

bool sameEntry(const EntryRef& a, const EntryRef& b) {
    return a == b || *a == *b;
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

// One entry of the diff: its kind, closed flag, the length of text `had` holds of it (non-zero only for an entry `had`
// holds open that only grew) and the rest of its text. In schema's tables (`tables`) an entry whose text is not a
// table's text goes whole, offset 0: the window holds such text with U+FFFD in place of the bytes it could not carry
// (codec::tableText), so an offset into it would not be where the server's is. Both texts are checked: the one held can
// be cut inside a character that the new one completes ("ab\xC3" growing to "ab\xC3\xA9"), and the window holds the cut
// text with U+FFFD, three bytes where the server's had one, so a growth from it would be refused on every later diff.
codec::EntryChange entryChange(const SessionEntry& e, const EntryRef* had, bool tables) {
    std::size_t offset = 0;
    if (had != nullptr && !(*had)->closed && (*had)->kind == e.kind && (*had)->text.size() <= e.text.size() &&
            e.text.compare(0, (*had)->text.size(), (*had)->text) == 0 &&
            (!tables || (codec::tableText((*had)->text) && codec::tableText(e.text)))) {
        offset = (*had)->text.size();
    }
    return codec::EntryChange{e.kind, e.closed, offset, e.text.substr(offset), e.accountEmailEnd};
}

// One changed session: what it is, its activity, its first index, the index k of the first entry `a` does not hold as
// it is, the entries from k to the end, and its queue if that changed.
codec::SessionChange sessionChange(const SessionView& b, const SessionView* a, bool tables) {
    codec::SessionChange c;
    c.id = b.id;
    c.changed = true;
    c.info = b.info;
    c.activity = b.activity;
    c.activeAgents = b.activeAgents;
    if (tables) c.busSenders = b.busSenders;
    c.first = b.first;
    std::uint64_t k = b.first;
    while (k < b.total()) {
        const EntryRef* had = entryAt(a, k);
        if (had == nullptr || !sameEntry(*had, b.entries[static_cast<std::size_t>(k - b.first)])) {
            break;
        }
        ++k;
    }
    c.keepFrom = k;
    c.entries.reserve(static_cast<std::size_t>(b.total() - k));
    for (std::uint64_t i = k; i < b.total(); ++i) {
        c.entries.push_back(entryChange(*b.entries[static_cast<std::size_t>(i - b.first)], entryAt(a, i), tables));
    }
    c.queueChanged = a == nullptr || a->queue != b.queue;
    if (c.queueChanged) {
        c.queue = b.queue;
    }
    return c;
}

// A changed session as read, applied to `a`, what this state holds of it (or null): the entries it keeps must be ones
// `a` holds, and an entry that grows must grow from one `a` holds open at that length.
SessionRef applySession(codec::SessionChange&& c, const SessionView* a) {
    auto t = std::make_shared<SessionView>();
    t->id = c.id;
    const std::string name = named(c.id);
    t->info = std::move(c.info);
    t->activity = c.activity;
    t->activeAgents = c.activeAgents;
    t->busSenders = std::move(c.busSenders);
    t->first = c.first;
    const std::uint64_t k = c.keepFrom;
    const std::uint64_t count = c.entries.size();
    if (k < t->first) {
        throw BadDiff(name + ": diff sends from entry " + std::to_string(k) + ", before its tail's first, " +
            std::to_string(t->first));
    }
    // Entry numbers past 2^64 - 1 would wrap: the session's length, k + count, must be one a u64 holds.
    if (count > ~std::uint64_t(0) - k) {
        throw BadDiff(name + ": diff sends " + std::to_string(count) + " entries from entry " + std::to_string(k) +
            ", past the last entry number there can be");
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
    std::uint64_t i = k;
    for (codec::EntryChange& e : c.entries) {
        std::string text = std::move(e.tail);
        if (e.offset != 0) {
            const EntryRef* had = entryAt(a, i);
            if (had == nullptr || (*had)->closed || (*had)->kind != e.kind || (*had)->text.size() != e.offset) {
                throw BadDiff(name + ": entry " + std::to_string(i) + " grows from " + std::to_string(e.offset) +
                    " bytes, which this state does not hold open");
            }
            text = (*had)->text + text;
        }
        t->entries.push_back(std::make_shared<const SessionEntry>(SessionEntry{e.kind, std::move(text), e.closed, e.accountEmailEnd}));
        ++i;
    }
    if (!c.queueChanged) {
        if (a == nullptr) {
            throw BadDiff(name + ": diff keeps the queue of a session this state lacks");
        }
        t->queue = a->queue;
    } else {
        t->queue = std::move(c.queue);
    }
    return t;
}

const std::shared_ptr<const std::vector<SecretRequest>>& noRequests() {
    static const auto none = std::make_shared<const std::vector<SecretRequest>>();
    return none;
}

const std::shared_ptr<const std::vector<ServerFriend>>& noFriends() {
    static const auto none = std::make_shared<const std::vector<ServerFriend>>();
    return none;
}

const std::shared_ptr<const std::vector<SignInOffer>>& noOffers() {
    static const auto none = std::make_shared<const std::vector<SignInOffer>>();
    return none;
}

} // namespace

void Sessions::setRequests(std::vector<SecretRequest> requests) {
    requests_ = requests.empty() ? noRequests() :
        std::make_shared<const std::vector<SecretRequest>>(std::move(requests));
}

void Sessions::setFriends(std::vector<ServerFriend> friends) {
    friends_ = friends.empty() ? noFriends() : std::make_shared<const std::vector<ServerFriend>>(std::move(friends));
    friendsKnown_ = true;
}

void Sessions::clearFriends() {
    friends_ = noFriends();
    friendsKnown_ = false;
}

void Sessions::shareFriends(const Sessions& from) {
    friends_ = from.friends_;
    friendsKnown_ = from.friendsKnown_;
}

void Sessions::setOffers(std::vector<SignInOffer> offers) {
    offers_ = offers.empty() ? noOffers() : std::make_shared<const std::vector<SignInOffer>>(std::move(offers));
}

bool SessionView::operator==(const SessionView& x) const {
    if (id != x.id || !(info == x.info) || activity != x.activity || activeAgents != x.activeAgents || first != x.first ||
            entries.size() != x.entries.size() || queue != x.queue || busSenders != x.busSenders) {
        return false;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!sameEntry(entries[i], x.entries[i])) {
            return false;
        }
    }
    return true;
}

bool visibleBus(std::string_view text, const SessionView& view) {
    if (text.empty()) return false;  // a redacted slot, still counted in raw transcript indexes
    if (!view.busSenders) return true;
    const auto separator = text.find('\n');
    return separator != text.npos && std::binary_search(view.busSenders->begin(), view.busSenders->end(),
                                                        text.substr(0, separator));
}

SessionView tailOf(SessionId id, SessionInfo info, const std::vector<EntryRef>& transcript,
                   std::vector<std::string> queue, Tail tail, EntryRef open,
                   std::optional<std::vector<std::string>> busSenders) {
    SessionView t;
    t.id = id;
    t.info = std::move(info);
    t.queue = std::move(queue);
    t.busSenders = std::move(busSenders);
    // The tail of the closed entries, then the open entry, if any, which the tail's rules do not count: so a state
    // always carries the newest closed entry, and a window that missed entries always gets one after its notice.
    const std::size_t count = transcript.size();
    std::size_t from = count;
    std::size_t bytes = 0;
    while (from > 0) {
        EntryRef e = transcript[from - 1];
        if (e->kind == EntryKind::Bus && !visibleBus(e->text, t)) {
            // Preserve the durable position without sending any part of this event to a window.
            static const EntryRef hidden = std::make_shared<const SessionEntry>(SessionEntry{EntryKind::Bus, {}, true});
            e = hidden;
        }
        const std::size_t size = e->text.size();
        const bool newest = from == count;
        if (!newest && (count - from >= tail.entries || bytes + size > tail.bytes)) {
            break;
        }
        bytes += size;
        t.entries.push_back(std::move(e));
        --from;
    }
    t.first = from;
    std::reverse(t.entries.begin(), t.entries.end());
    if (open) {
        t.entries.push_back(std::move(open));
    }
    return t;
}

Sessions::Sessions()
    : sessions_(std::make_shared<const std::vector<SessionRef>>()), requests_(noRequests()), friends_(noFriends()),
      offers_(noOffers()) {}

Sessions::Sessions(std::vector<SessionRef> sessions)
    : requests_(noRequests()), friends_(noFriends()), offers_(noOffers()) {
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i]->id == 0) {
            throw std::logic_error("Sessions: a session with id 0");
        }
        if (i > 0 && !(sessions[i - 1]->id < sessions[i]->id)) {
            throw std::logic_error("Sessions: sessions out of id order or with one id twice: " +
                std::to_string(sessions[i - 1]->id) + ", " + std::to_string(sessions[i]->id));
        }
        if (sessions[i]->info.friendName == kPrimaryChat && primaryChat_ == 0) primaryChat_ = sessions[i]->id;
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

// A diff: for each session in id order, its id and whether `existing` holds it as it is; if not, the session
// (sessionChange). A session `existing` holds and this state lacks is not named, which removes it. Then the open secret
// requests, and the server's friends when it set them (a format before 4 carries none). Written in this state's format
// (sync/codec.h).
std::string Sessions::diff_from(const Sessions& existing) const {
    if (*this == existing) {
        return std::string();
    }
    const bool tables = format_ >= kTablesFormat;
    codec::SessionsChange change;
    change.sessions.reserve(sessions_->size());
    for (const SessionRef& b : *sessions_) {
        const SessionView* a = existing.find(b->id);
        if (a != nullptr && (a == b.get() || *a == *b)) {
            codec::SessionChange kept;
            kept.id = b->id;
            change.sessions.push_back(std::move(kept));
            continue;
        }
        change.sessions.push_back(sessionChange(*b, a, tables));
    }
    change.requests = *requests_;
    // The friends only when they changed: the reader keeps the list it holds otherwise, so an unchanged list costs no
    // bytes however long it is. The hub shares one list between its states until a reload sets another, so the
    // comparison is of one pointer in the usual case.
    if (friendsKnown_ != existing.friendsKnown_ ||
            (friends_ != existing.friends_ && *friends_ != *existing.friends_)) {
        change.friendsSent = true;
        change.friendsKnown = friendsKnown_;
        change.friends = *friends_;
    }
    // The sign-in flows ride only on a link of 5 or later (sync/formats.h); a window below it gets none.
    if (format_ >= kSignInFormat) {
        change.offers = *offers_;
        change.nextAction = nextAction_;
    }
    return tables ? codec::writeTables(change, format_) : codec::writeLegacy(change, format_);
}

std::string Sessions::init_diff() const {
    return diff_from(Sessions());
}

void Sessions::apply_string(const std::string& diff) {
    if (diff.empty()) {
        return;
    }
    int unknown = 0;
    codec::SessionsChange change = format_ >= kTablesFormat ? codec::readTables(diff, &unknown) :
        codec::readLegacy(diff, format_);
    if (format_ < kSignInFormat) {
        change.offers.clear();      // taken only on a link of 5 or later (sync/formats.h)
    }
    std::vector<SessionRef> sessions;
    sessions.reserve(change.sessions.size());
    SessionId primary = 0;
    for (codec::SessionChange& c : change.sessions) {
        const SessionId id = c.id;
        if (id == 0) {
            throw BadDiff("sessions diff names session 0; ids start at 1");
        }
        if (!sessions.empty() && !(sessions.back()->id < id)) {
            throw BadDiff("sessions diff names " + named(id) + " out of order or twice");
        }
        auto had = lookup(*sessions_, id);
        const SessionView* a = had == sessions_->end() ? nullptr : had->get();
        if (!c.changed) {
            if (a == nullptr) {
                throw BadDiff("sessions diff keeps " + named(id) + ", which this state lacks");
            }
            sessions.push_back(*had);
        } else {
            sessions.push_back(applySession(std::move(c), a));
        }
        if (sessions.back()->info.friendName == kPrimaryChat && primary == 0) primary = id;
    }
    for (std::size_t i = 0; i < change.requests.size(); ++i) {
        const SecretRequest& q = change.requests[i];
        if (q.id == 0 || (i > 0 && !(change.requests[i - 1].id < q.id))) {
            throw BadDiff("sessions diff names secret request " + std::to_string(q.id) + " out of order or twice");
        }
    }
    for (std::size_t i = 0; i < change.offers.size(); ++i) {
        const SignInOffer& o = change.offers[i];
        if (o.id == 0 || (i > 0 && !(change.offers[i - 1].id < o.id))) {
            throw BadDiff("sessions diff names sign-in " + std::to_string(o.id) + " out of order or twice");
        }
    }
    sessions_ = std::make_shared<const std::vector<SessionRef>>(std::move(sessions));
    primaryChat_ = primary;
    setRequests(std::move(change.requests));
    if (change.friendsSent && change.friendsKnown) {
        setFriends(std::move(change.friends));
    } else if (change.friendsSent) {
        clearFriends();
    }
    setOffers(std::move(change.offers));
    nextAction_ = change.nextAction;
    unknown_ = unknown;
}

bool Sessions::operator==(const Sessions& x) const {
    if (nextAction_ != x.nextAction_) return false;
    if (requests_ != x.requests_ && *requests_ != *x.requests_) {
        return false;
    }
    if (friendsKnown_ != x.friendsKnown_ || (friends_ != x.friends_ && *friends_ != *x.friends_)) {
        return false;
    }
    if (offers_ != x.offers_ && *offers_ != *x.offers_) {
        return false;
    }
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
    // No sign-in goes in a file: an offer's URL is for the window holding it alone.
    Sessions kept = state;
    kept.setOffers({});
    kept.setNextAction(0);
    return kStateFileTag + std::to_string(state.format()) + "\n" + kept.init_diff();
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
    if (format < kMinPeer) {
        *why = "a state of format " + std::to_string(format) + ", older than this build reads (" +
            std::to_string(kMinPeer) + " on)";
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
