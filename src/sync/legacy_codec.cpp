// Formats 1 to 3 (sync/formats.h, docs/formats.md): the hand-written codecs from before schema's tables, kept byte for
// byte for windows and servers from before format 4, and never extended. Unsigned varints and length-prefixed strings
// (sync/wire.h).

#include "sync/codec.h"

namespace nexus::sync::codec {

namespace {

std::string named(SessionId id) {
    return "session " + std::to_string(id);
}

// EntryKind on the wire: Person 0, AI 1, Notice 2, Tool 3 (from format 2). Format 1 has no Tool: a tool line goes
// to a window of format 1 as the AI's text, as servers before format 2 sent it. A kind this build does not know (a
// later server's) is read as a notice, which the window draws as a plain dim line.
std::uint8_t kindOnWire(EntryKind k, std::uint32_t format) {
    if (k == EntryKind::Bus) return static_cast<std::uint8_t>(EntryKind::Notice);
    if (format < 2 && k == EntryKind::Tool) {
        return static_cast<std::uint8_t>(EntryKind::AI);
    }
    return static_cast<std::uint8_t>(k);
}

EntryKind kindRead(std::uint8_t k) {
    return k <= static_cast<std::uint8_t>(EntryKind::Tool) ? static_cast<EntryKind>(k) : EntryKind::Notice;
}

// Activity on the wire, from format 2: Idle 0, Busy 1, Down 2. Format 1 carries none, and a session of it reads idle:
// that server cannot say, and idle claims least (the person may send, which is always safe, where busy would have them
// wait on nothing). A value this build does not know (a later server's) reads busy: the server said the AI is
// neither idle nor down, so not waiting on the person.
Activity activityRead(std::uint8_t a) {
    return a <= static_cast<std::uint8_t>(Activity::Down) ? static_cast<Activity>(a) : Activity::Busy;
}

// One entry: its kind, closed flag, offset, and the rest of its text. From format 2 these are framed by their length,
// and fields a later format adds go after them.
void writeEntry(Writer& w, const EntryChange& e, std::uint32_t format) {
    Writer body;
    Writer& out = format < 2 ? w : body;
    out.byte(kindOnWire(e.kind, format));
    out.byte(e.closed ? 1 : 0);
    out.u64(e.offset);
    out.str(e.tail);
    if (format >= 2) {
        w.str(body.take());
    }
}

// One changed session: what it is (its AI, harness, account, model and effort); from format 2 its activity; its
// first index; keepFrom; the entries from there to the end; then a byte, 1 if the queue changed, and if so the queue.
// From format 2 the whole is framed by its length, and fields added since go after the queue, each only ever
// appended: its instance (empty: the AI's default instance). A window of format 1 is not sent the instance.
void writeSession(Writer& w, const SessionChange& s, std::uint32_t format) {
    Writer body;
    Writer& out = format < 2 ? w : body;
    out.str(s.info.aiName);
    out.str(s.info.harness);
    out.str(s.info.account);
    out.str(s.info.model);
    out.str(s.info.effort);
    if (format >= 2) {
        out.byte(static_cast<std::uint8_t>(s.activity));
    }
    out.u64(s.first);
    out.u64(s.keepFrom);
    out.u64(s.entries.size());
    for (const EntryChange& e : s.entries) {
        writeEntry(out, e, format);
    }
    out.byte(s.queueChanged ? 1 : 0);
    if (s.queueChanged) {
        out.u64(s.queue.size());
        for (const std::string& q : s.queue) {
            out.str(q);
        }
    }
    if (format >= 2) {
        out.str(s.info.instance);
        w.str(body.take());
    }
}

// Entry i, read from `r` (format 1) or from its frame (format 2 on, whose fields past the text are skipped).
EntryChange readEntry(Reader& r, const std::string& name, std::uint64_t i, std::uint32_t format) {
    std::string frame;
    if (format >= 2) {
        frame = r.str();
    }
    Reader framed(frame);
    Reader& in = format < 2 ? r : framed;
    EntryChange e;
    e.kind = kindRead(in.byte());
    const std::uint8_t closed = in.byte();
    if (closed > 1) {
        throw BadDiff(name + ": entry " + std::to_string(i) + " has closed flag " + std::to_string(closed));
    }
    e.closed = closed == 1;
    e.offset = in.u64();
    e.tail = in.str();
    return e;
}

// A changed session's fields, from `r`; `left` bounds the counts it may declare (the bytes there are to read).
SessionChange readFields(Reader& r, std::size_t left, SessionId id, std::uint32_t format) {
    SessionChange s;
    s.id = id;
    s.changed = true;
    const std::string name = named(id);
    s.info.aiName = r.str();
    s.info.harness = r.str();
    s.info.account = r.str();
    s.info.model = r.str();
    s.info.effort = r.str();
    s.activity = format >= 2 ? activityRead(r.byte()) : Activity::Idle;
    s.first = r.u64();
    s.keepFrom = r.u64();
    const std::uint64_t count = r.u64();
    // Each entry takes at least one byte, so a count past what is left of the diff is malformed.
    if (count > left) {
        throw BadDiff(name + ": diff says it sends " + std::to_string(count) + " entries in " + std::to_string(left) +
            " bytes");
    }
    s.entries.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        s.entries.push_back(readEntry(r, name, s.keepFrom + i, format));
    }
    const std::uint8_t queueChanged = r.byte();
    if (queueChanged == 1) {
        s.queueChanged = true;
        const std::uint64_t n = r.u64();
        if (n > left) {
            throw BadDiff(name + ": diff says its queue holds " + std::to_string(n) + " prompts in " +
                std::to_string(left) + " bytes");
        }
        for (std::uint64_t q = 0; q < n; ++q) {
            s.queue.push_back(r.str());
        }
    } else if (queueChanged != 0) {
        throw BadDiff(name + ": queue flag " + std::to_string(queueChanged) + ", neither 0 nor 1");
    }
    // From format 2, the fields appended after the queue; a frame from before them gives their defaults.
    if (format >= 2 && !r.done()) {
        s.info.instance = r.str();
    }
    return s;
}

// A changed session: in format 1 read straight from the diff; from format 2 from its frame, whose fields past the
// queue (a later format's) are skipped.
SessionChange readSession(Reader& r, std::size_t left, SessionId id, std::uint32_t format) {
    if (format < 2) {
        return readFields(r, left, id, format);
    }
    const std::string frame = r.str();
    Reader framed(frame);
    return readFields(framed, frame.size(), id, format);
}

// The open secret requests, from format 3, after the sessions: how many, then each framed by its length (its id,
// account, what is asked, the asker's session and AI, and the asker's reason; fields a later format adds go after
// them).
void writeRequests(Writer& w, const std::vector<SecretRequest>& requests) {
    w.u64(requests.size());
    for (const SecretRequest& q : requests) {
        Writer f;
        f.u64(q.id);
        f.str(q.account);
        f.str(q.what);
        f.u64(q.asker);
        f.str(q.askerName);
        f.str(q.why);
        w.str(f.take());
    }
}

std::vector<SecretRequest> readRequests(Reader& r, std::size_t left) {
    const std::uint64_t n = r.u64();
    if (n > left) {
        throw BadDiff("sessions diff says it holds " + std::to_string(n) + " secret requests in " +
            std::to_string(left) + " bytes");
    }
    std::vector<SecretRequest> out;
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::string frame = r.str();
        Reader f(frame);
        SecretRequest q;
        q.id = f.u64();
        q.account = f.str();
        q.what = f.str();
        q.asker = f.u64();
        q.askerName = f.str();
        q.why = f.str();
        out.push_back(std::move(q));
    }
    return out;
}

bool isSecret(ActionKind kind) {
    return kind == ActionKind::SecretAnswer || kind == ActionKind::SecretCancel;
}

// The sign-in's kinds (format 5 on, sync/signin.h), which these formats never carry: a window sends one only for a
// flow it received, which a server sends only on a link of 5 or later.
bool isSignIn(ActionKind kind) {
    return kind == ActionKind::SignOut || kind == ActionKind::SignInClaim ||
        kind == ActionKind::SignInOpened || kind == ActionKind::SignInCallback || kind == ActionKind::SignInCancel;
}

// One action in format 3: its kind, then its fields framed by their length (its session, AI, text, account,
// request and value), so that a later format appends fields and a reader skips what it does not know. A sign-in's
// action goes as a stop naming session 0 and nothing else, which every server refuses and counts, so that the numbers
// stay unbroken and nothing of the sign-in leaves on a link of 1 to 3.
void encodeFramed(Writer& w, const Action& a) {
    if (isSignIn(a.kind)) {
        Writer stop;
        stop.u64(0);
        stop.str(std::string());
        stop.str(std::string());
        stop.str(std::string());
        stop.u64(0);
        stop.str(std::string());
        w.byte(static_cast<std::uint8_t>(ActionKind::Stop));
        w.str(stop.take());
        return;
    }
    Writer frame;
    frame.u64(a.session);
    frame.str(a.aiName);
    frame.str(a.text);
    frame.str(a.account);
    frame.u64(a.request);
    frame.str(a.secret.bytes());
    w.byte(static_cast<std::uint8_t>(a.kind));
    std::string bytes = frame.take();
    w.str(bytes);
    wipeBytes(bytes);   // the frame's copy of a value is wiped as it goes
}

// One action up to format 2: unframed. A secret's or a sign-in's action, which these formats cannot carry, goes as a
// stop naming session 0, which every server refuses and counts, so that the numbers stay unbroken and the value never
// leaves.
void encodeUnframed(Writer& w, const Action& a) {
    if (isSecret(a.kind) || isSignIn(a.kind) || a.kind == ActionKind::ResolveLimit) {
        w.byte(static_cast<std::uint8_t>(ActionKind::Stop));
        w.u64(0);
        w.str(std::string());
        w.str(std::string());
        return;
    }
    w.byte(static_cast<std::uint8_t>(a.kind));
    w.u64(a.session);
    w.str(a.aiName);
    w.str(a.text);
    if (a.kind >= ActionKind::AddAccount) {
        w.str(a.account);    // only the account actions carry it, so the older kinds' bytes are as they were
    }
}

} // namespace

// A diff: the number of sessions; then for each, in id order, its id and a byte: 0 when the state diffed from holds it
// as it is, else 1 and the session (writeSession). From format 3 the open secret requests follow.
std::string writeLegacy(const SessionsChange& change, std::uint32_t format) {
    Writer w;
    w.u64(change.sessions.size());
    for (const SessionChange& s : change.sessions) {
        w.u64(s.id);
        if (!s.changed) {
            w.byte(0);
            continue;
        }
        w.byte(1);
        writeSession(w, s, format);
    }
    if (format >= kSecretsFormat) {
        writeRequests(w, change.requests);
    }
    return w.take();
}

SessionsChange readLegacy(const std::string& diff, std::uint32_t format) {
    SessionsChange out;
    Reader r(diff);
    const std::uint64_t n = r.u64();
    if (n > diff.size()) {
        throw BadDiff("sessions diff says it holds " + std::to_string(n) + " sessions in " +
            std::to_string(diff.size()) + " bytes");
    }
    out.sessions.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
        const SessionId id = r.u64();
        const std::uint8_t changed = r.byte();
        if (changed == 0) {
            SessionChange kept;
            kept.id = id;
            out.sessions.push_back(std::move(kept));
        } else if (changed == 1) {
            out.sessions.push_back(readSession(r, diff.size(), id, format));
        } else {
            throw BadDiff("sessions diff marks " + named(id) + " with " + std::to_string(changed) +
                ", neither 0 nor 1");
        }
    }
    // Format 1 ends with its sessions; from format 2 what a later format adds after them is skipped. From format 3 the
    // open secret requests follow, always (a diff cut short before them is refused, not read as none), and what a
    // later format adds after them is skipped.
    if (format < 2) {
        r.end();
    }
    if (format >= kSecretsFormat) {
        out.requests = readRequests(r, diff.size());
    }
    return out;
}

// A diff: the number of its first action, how many follow, and each action, in the link's format. The numbers of the
// rest follow from the first, so a diff is always one unbroken run.
std::string writeActionsLegacy(std::uint64_t start, const std::vector<Action>& held, std::size_t from,
                               std::uint32_t format) {
    Writer w;
    w.u64(start);
    w.u64(held.size() - from);
    for (std::size_t i = from; i < held.size(); ++i) {
        if (format >= 3) {
            encodeFramed(w, held[i]);
        } else {
            encodeUnframed(w, held[i]);
        }
    }
    return w.take();
}

std::vector<Action> readActionsLegacy(const std::string& diff, std::uint32_t format) {
    std::vector<Action> out;
    Reader r(diff);
    const std::uint64_t start = r.u64();
    const std::uint64_t count = r.u64();
    if (start == 0)
        throw BadDiff("actions diff starts at number 0; numbers start at 1");
    if (count == 0 || count > diff.size())
        throw BadDiff("actions diff says it holds " + std::to_string(count) + " actions in " +
            std::to_string(diff.size()) + " bytes");
    if (start + count < start)
        throw BadDiff("actions diff numbers run past 64 bits");
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        Action a;
        a.num = start + i;
        const std::uint8_t kind = r.byte();
        if (format >= 3) {
            // Framed: a kind this build does not know is read as Unknown, its fields skipped with its frame.
            std::string frame = r.str();
            if (kind == 0 || kind > static_cast<std::uint8_t>(ActionKind::SecretCancel)) {
                a.kind = ActionKind::Unknown;
                wipeBytes(frame);
                out.push_back(std::move(a));
                continue;
            }
            a.kind = static_cast<ActionKind>(kind);
            Reader f(frame);
            a.session = f.u64();
            a.aiName = f.str();
            a.text = f.str();
            a.account = f.str();
            a.request = f.u64();
            std::string value = f.str();
            a.secret = SecretValue(value);
            wipeBytes(value);
            wipeBytes(frame);
            out.push_back(std::move(a));
            continue;
        }
        if (kind < static_cast<std::uint8_t>(ActionKind::Send) ||
                kind > static_cast<std::uint8_t>(ActionKind::SetDefault)) {
            throw BadDiff("actions diff holds action kind " + std::to_string(kind) + ", none of send (1) to set "
                "default (14)");
        }
        a.kind = static_cast<ActionKind>(kind);
        a.session = r.u64();
        a.aiName = r.str();
        a.text = r.str();
        if (a.kind >= ActionKind::AddAccount) {
            a.account = r.str();
        }
        out.push_back(std::move(a));
    }
    r.end();
    return out;
}

} // namespace nexus::sync::codec
