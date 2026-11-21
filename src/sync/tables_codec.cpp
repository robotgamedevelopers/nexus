// Format 4 on (sync/formats.h, docs/formats.md): the state and the actions as schema's variable tables, SessionsDiff and
// ActionsDiff of schema/Nexus.schema, through the generated code (schema/gen, src/wire/tables.h). Nothing here encodes
// a byte by hand: this file and tables_actions.cpp copy between nexus's own types and the generated ones. This file
// holds the sessions and the text rule, tables_actions.cpp the actions, tables_shared.h what they share; two files so
// that they compile side by side.
//
// What crosses versions, by the tables' own rules: a field rides under the hash of its name, so a reader skips one it
// does not know (counted in report.unknown) and reads one an older writer left out as its declared default; an enum
// value it does not know reads None, counted too. A read refuses only bytes that do not load (damaged, cut short, or a
// form this build does not carry).
//
// Text: a table's text is well-formed UTF-8 with no zero byte, and a reader refuses the whole diff otherwise. nexus's
// strings are bytes, so text that breaks the rule is written with each offending byte as U+FFFD (tableText).

#include "sync/tables_shared.h"

namespace nexus::sync::codec {

namespace {

using namespace shared;

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

} // namespace

bool tableText(std::string_view text) {
    return wire::TableUtf8Valid(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void checkDiffSize(std::size_t diffBytes, const char* what) {
    if (diffBytes > kMaxDiffBytes) {
        throw BadDiff(std::string(what) + " of " + std::to_string(diffBytes) + " bytes is longer than the link carries ("
            + std::to_string(kMaxDiffBytes) + ")");
    }
}

void checkReadSize(std::size_t diffBytes, std::int64_t need, const char* what) {
    if (need < 0) {
        throw BadDiff(std::string(what) + " of " + std::to_string(diffBytes) + " bytes does not load as schema's "
            "tables (damaged, cut short, or a form this build does not read)");
    }
    if (static_cast<std::uint64_t>(need) > kMaxReadBytes) {
        throw BadDiff(std::string(what) + " of " + std::to_string(diffBytes) + " bytes would load into " +
            std::to_string(need) + " bytes, past the " + std::to_string(kMaxReadBytes) + " one read allocates");
    }
}

std::string writeTables(const SessionsChange& change, std::uint32_t format) {
    // The flows only on a link of 5 or later (sync/formats.h). A URL is in an offer only in the state of the window
    // that holds it (Hub::stateFor), and it is the one sign-in secret this writer ever puts in a table: the builder
    // then wipes its memory as an actions diff with a value does.
    const bool signIns = format >= kSignInFormat;
    bool url = false;
    for (const SignInOffer& o : change.offers) {
        url = url || (signIns && !o.url.empty());
    }
    const wire::TableAllocator wipes = wiping();
    wire::SessionsDiffBuilder b(url ? wipes : wire::TableDefaultAllocator());
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
        x->info.limit_request = s.info.limitRequest;
        putText(b.main, x->info.limit_account, s.info.limitAccount, scratch);
        putText(b.main, x->info.limit_model, s.info.limitModel, scratch);
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
    if (change.friendsSent) {
        d->friends_sent = true;
        d->friends_known = change.friendsKnown;
        for (const ServerFriend& f : change.friends) {
            wire::Friend* w = added(wire::SessionsDiffFriendsAdd(b.main, d->friends));
            putText(b.main, w->name, f.name, scratch);
            for (const AccountChoice& account : f.accounts) {
                auto* choice = added(wire::FriendAccountsAdd(b.main, w->accounts));
                putText(b.main, choice->name, account.name, scratch);
                putText(b.main, choice->label, account.label, scratch);
                choice->needs_sign_in = account.needsSignIn;
                putText(b.main, choice->email, account.email, scratch);
            }
        }
    }
    if (signIns) {
        d->next_action = change.nextAction;
        for (const SignInOffer& o : change.offers) {
            wire::SignInOffer* x = added(wire::SessionsDiffSigninsAdd(b.main, d->signins));
            x->id = o.id;
            putText(b.main, x->account, o.account, scratch);
            putText(b.main, x->provider, o.provider, scratch);
            putText(b.main, x->opener, o.opener, scratch);
            putText(b.main, x->host, o.host, scratch);
            putBytes(b.main, x->url, o.url.bytes());
        }
    }
    return saved(b, [](const wire::SessionsDiffBuilder& x) { return wire::SessionsDiffMeasure(x); },
        [](const wire::SessionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return wire::SessionsDiffSave(x, out, n); },
        "sessions diff");
}

bool rosterFits(std::vector<ServerFriend> friends) {
    SessionsChange state;
    state.friendsSent = true;
    state.friendsKnown = true;
    state.friends = std::move(friends);
    const std::string bytes = writeTables(state, kStateFormat);
    if (bytes.size() > kRosterWireBytes) {
        return false;
    }
    const auto need = wire::SessionsDiffLoadMeasure(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return need > 0 && static_cast<std::size_t>(need) <= kRosterReadBytes;
}

SessionsChange readTables(const std::string& diff, int* unknown) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(diff.data());
    const int64_t size = static_cast<int64_t>(diff.size());
    const std::size_t space = readNeed(diff, [](const std::uint8_t* b, int64_t n) {
        return wire::SessionsDiffLoadMeasure(b, n);
    }, "sessions diff");
    const int64_t need = static_cast<int64_t>(space);
    Region region;
    region.bytes.resize(space);
    wire::TableReport report;
    const wire::SessionsDiff* d = wire::SessionsDiffLoad(region.bytes.data(), need, bytes, size, &report);
    if (d == nullptr || report.damaged || report.refused) {
        throw BadDiff("sessions diff of " + std::to_string(diff.size()) + " bytes does not load as schema's "
            "SessionsDiff (damaged, cut short, or a form this build does not read)");
    }
    *unknown = report.unknown;
    SessionsChange out;
    out.nextAction = d->next_action;
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
            s.info.limitRequest = x.info.limit_request;
            s.info.limitAccount = textOf(x.info.limit_account);
            s.info.limitModel = textOf(x.info.limit_model);
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
    // Only a diff that says it carries the friends sets them: without friends_sent the reader keeps what it held, and any
    // friends such a diff names are ignored.
    out.friendsSent = d->friends_sent;
    if (out.friendsSent) {
        out.friendsKnown = d->friends_known;
        out.friends.reserve(static_cast<std::size_t>(d->friends.size()));
        for (const wire::Friend& f : d->friends) {
            ServerFriend friendState{textOf(f.name)};
            friendState.accounts.reserve(static_cast<std::size_t>(f.accounts.size()));
            for (const wire::AccountChoice& choice : f.accounts) {
                friendState.accounts.push_back(
                    {textOf(choice.name), textOf(choice.label), choice.needs_sign_in, textOf(choice.email)});
            }
            out.friends.push_back(std::move(friendState));
        }
    }
    for (const wire::SignInOffer& x : d->signins) {
        SignInOffer o;
        o.id = x.id;
        o.account = textOf(x.account);
        o.provider = textOf(x.provider);
        o.opener = textOf(x.opener);
        o.host = textOf(x.host);
        o.url = bytesOf(x.url);
        out.offers.push_back(std::move(o));
    }
    return out;
}

} // namespace nexus::sync::codec
