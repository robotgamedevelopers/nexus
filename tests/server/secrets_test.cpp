// Tests of the secret dialog (docs/secrets.md): the server's desk and the window's prompt (src/sync/secrets.h), their
// codecs (an action up in formats 3 and 2, the requests down in the state), the hub's asks and answers over the
// server's store (server/durable.h), the account's file (server/secret_files.h) at every crash point, and the leak
// test: one fake value pasted, answered, answered again late and cancelled across three windows, an asker's driver and a
// restart, and then every output searched for its bytes.
//
// Unit groups: logic only, no time (the clock is an argument), no sockets, no files (the store and the accounts are
// MemFs). `files_disk` is functional: it writes through the real filesystem under ./secrets_disk in its working
// directory to check the modes (0600 for the file, 0700 for its directory), the refusal of a wide or linked account
// directory, a temporary file never written through a link, and the start's sweep. Every value is made up here: never
// a real token.
//
//   secrets_test            runs every unit group
//   secrets_test <group>    runs one

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "driver/protocol.h"
#include "server/durable.h"
#include "server/login_life.h"
#include "leak_scan.h"
#include "server/secret_files.h"
#include "server/secret_lines.h"
#include "store/fs.h"
#include "sync/actions.h"
#include "sync/formats.h"
#include "sync/hub.h"
#include "sync/secrets.h"
#include "sync/sessions.h"
#include "sync/wire.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

// The fake values: made up here, long enough that no other text holds them by chance.
const std::string kValue = "fakeQm8vTrXw" + std::string(64, 'z');
const std::string kOther = "fakePw3kGdHb" + std::string(64, 'y');
const char* const kToken = "oauth-token";

bool holds(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Whether `text` gives either fake value away: the value, a run of 8 of its bytes, its hex or base64, or its length
// beside an account's name (leak_scan.h).
bool leaks(const std::string& text) {
    static const std::vector<test::LeakScan> scans{test::LeakScan(kValue, "a1"), test::LeakScan(kValue, "a2"),
                                                   test::LeakScan(kOther, "a1"), test::LeakScan(kOther, "a2")};
    return std::any_of(scans.begin(), scans.end(), [&](const test::LeakScan& s) { return s.in(text); });
}

sync::SecretAsker asker(sync::SessionId session, std::uint64_t ask) {
    return sync::SecretAsker{session, ask, session == 0 ? std::string() : std::string("f")};
}

// The account's file, in memory.
struct Keeper : sync::SecretKeeper {
    std::map<std::string, std::string> files;
    std::size_t writes = 0;
    bool fail = false;
    bool unconfirmed = false;
    sync::Kept keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
                    std::string* why) override {
        if (fail) {
            *why = "the disk is full";
            return sync::Kept::Failed;
        }
        files[account + "/" + what] = value.bytes();
        ++writes;
        if (unconfirmed) {
            *why = "the directory's sync failed";
            return sync::Kept::Unconfirmed;
        }
        return sync::Kept::Durable;
    }
};

struct Events : sync::SecretEvents {
    std::vector<sync::SecretReply> replies;
    std::vector<sync::SecretNote> notes;
    std::vector<sync::SecretLog> log;
    void answered(const sync::SecretReply& reply) override { replies.push_back(reply); }
    void logged(const sync::SecretLog& line) override { log.push_back(line); }
    void noted(const sync::SecretNote& note) override { notes.push_back(note); }
};

struct Responder : sync::Responder {
    std::vector<std::string> logins;
    sync::SessionInfo defaults(const std::string& friendName) const override {
        return sync::SessionInfo{friendName, sync::kDefaultHarness, "", "", ""};
    }
    std::vector<sync::SessionChange> send(sync::SessionId, const std::string&) override { return {}; }
    std::vector<sync::SessionChange> queue(sync::SessionId, const std::string&) override { return {}; }
    std::vector<sync::SessionChange> stop(sync::SessionId) override { return {}; }
    void sent(sync::SessionId, bool) override {}
    void configured(sync::SessionId, const sync::SessionInfo&) override {}
    void signedIn(const std::string& account) override { logins.push_back(account); }
};

std::string readFile(store::Fs& fs, const std::string& path) {
    store::Bytes bytes;
    if (!fs.read_all(path, bytes).is_ok()) {
        return "(none)";
    }
    return std::string(bytes.begin(), bytes.end());
}

std::vector<std::string> names(store::Fs& fs, const std::string& dir) {
    std::vector<std::string> out;
    fs.list(dir, out);
    return out;
}

// nexus-server as the tests need it: its store on a MemFs, a hub with friend f's session (1) and the accounts a1 and a2
// (claude) and a3 (a provider that documents no secret), all three linked to f, and a4 (claude) linked to no one; and
// the secret dialog wired to `keeper`.
struct Server {
    server::DurableStore store;
    std::unique_ptr<sync::Hub> hub;
    Events events;
    Responder responder;
    bool takers = true;
    std::uint64_t now = 1000;

    Server(store::MemFs& fs, sync::SecretKeeper& keeper) {
        std::string why;
        CHECK(store.open(fs, "store", &why));
        hub = std::make_unique<sync::Hub>(store, sync::Limits{}, &responder);
        sync::Action ensure;
        ensure.kind = sync::ActionKind::Ensure;
        ensure.friendName = "f";
        hub->apply(ensure, &why);
        CHECK(store.find(1) != nullptr);
        accounts::Registry r;
        r.accounts = {accounts::Account{"a1", "claude"}, accounts::Account{"a2", "claude"},
                      accounts::Account{"a3", "other"}, accounts::Account{"a4", "claude"}};
        r.friends = {accounts::Friend{"f", {"a1", "a2", "a3"}, "a1"}};
        CHECK(hub->useAccounts(r, nullptr).empty());
        hub->secretsWith(&keeper, &events, [this] { return takers; }, [this] { return now; });
    }

    // What session `id`'s transcript holds, joined.
    std::string transcript(sync::SessionId id) const {
        std::string out;
        if (const sync::StoredSession* s = store.find(id)) {
            for (const sync::EntryRef& e : s->transcript) {
                out += e->text + "\n";
            }
        }
        return out;
    }
};

sync::Action answer(std::uint64_t request, const std::string& value) {
    sync::Action a;
    a.kind = sync::ActionKind::SecretAnswer;
    a.request = request;
    a.secret = sync::SecretValue(value);
    return a;
}

// `a`, numbered `num` in its window's list (the hub counts it under the window's name).
sync::Action numbered(std::uint64_t num, sync::Action a) {
    a.num = num;
    return a;
}

sync::Action cancel(std::uint64_t request) {
    sync::Action a;
    a.kind = sync::ActionKind::SecretCancel;
    a.request = request;
    return a;
}

// ---- the desk: each request answered once ----

void desk() {
    sync::SecretDesk d;
    Keeper k;
    sync::SecretEffects e = d.ask(1, asker(1, 1), "a1", kToken, "why", true, 1000);
    CHECK(e.changed && e.replies.empty() && d.open().size() == 1 && d.isOpen(1));
    CHECK(e.notes.size() == 1 && e.notes[0].what == "asked" && e.log.size() == 1 && e.log[0].event == "asked");
    CHECK(d.deadline() == 1000 + sync::kSecretDeadlineMs);
    // One open per account: a second ask for a1 is refused at once, busy.
    e = d.ask(2, asker(1, 2), "a1", kToken, "", true, 1001);
    CHECK(!e.changed && e.replies.size() == 1 && !e.replies[0].stored && e.replies[0].reason == sync::reason::kBusy);
    CHECK(d.stateOf(2) == sync::SecretState::Refused && d.open().size() == 1);
    // No window that can take it: no-client, at once.
    e = d.ask(3, asker(0, 0), "a2", kToken, "", false, 1002);
    CHECK(e.replies.size() == 1 && e.replies[0].reason == sync::reason::kNoClient && !d.isOpen(3));
    // The first answer is kept, and the asker hears stored, nothing more.
    e = d.answer(1, sync::SecretValue(kValue), k, 1000);
    CHECK(e.replies.size() == 1 && e.replies[0].stored && e.replies[0].reason.empty());
    CHECK(e.replies.size() == 1 && e.replies[0].asker == asker(1, 1) && e.replies[0].id == 1);
    CHECK(k.files["a1/oauth-token"] == kValue && d.stateOf(1) == sync::SecretState::Stored && e.changed);
    // A second answer, or a cancel, for it: late, nothing written, nobody answered again.
    e = d.answer(1, sync::SecretValue(kOther), k, 1000);
    CHECK(e.replies.empty() && k.writes == 1 && e.log.size() == 1 && e.log[0].event == "late" && !e.changed);
    e = d.cancel(1);
    CHECK(e.replies.empty() && e.log.size() == 1 && e.log[0].event == "late");
    // Escape: cancelled.
    d.ask(4, asker(1, 3), "a1", kToken, "", true, 2000);
    e = d.cancel(4);
    CHECK(e.replies.size() == 1 && e.replies[0].reason == sync::reason::kCancelled && e.changed);
    // The file could not be written: never refused. The value is held, the asker not answered yet, the dialogs closed,
    // and the keeper's reason in the log alone.
    d.ask(5, asker(1, 4), "a1", kToken, "", true, 3000);
    k.fail = true;
    e = d.answer(5, sync::SecretValue(kValue), k, 3000);
    CHECK(e.replies.empty() && e.changed && !d.isOpen(5) && d.stateOf(5) == sync::SecretState::Holding);
    CHECK(e.log.size() == 1 && e.log[0].event == "holding" && holds(e.log[0].detail, "the disk is full"));
    CHECK(e.notes.size() == 1 && e.notes[0].what == "holding" && holds(e.notes[0].text, "nothing needs pasting again"));
    CHECK(d.holding().size() == 1 && d.busy("a1") && d.waiting(1) && d.retryAt() == 3000 + sync::kSecretRetryFirstMs);
    // Held: a new ask for a1 is busy, and a second answer for 5 (another window's) is late.
    e = d.ask(51, asker(0, 0), "a1", kToken, "", true, 3001);
    CHECK(e.replies.size() == 1 && e.replies[0].reason == sync::reason::kBusy);
    e = d.answer(5, sync::SecretValue(kOther), k, 3001);
    CHECK(e.log.size() == 1 && e.log[0].event == "late" && k.files.count("a1/oauth-token") == 1 &&
          k.files["a1/oauth-token"] == kValue);
    // Not before its time; at its time, still failing: held again, twice as far off.
    e = d.retry(3999, k);
    CHECK(e.log.empty() && d.stateOf(5) == sync::SecretState::Holding);
    e = d.retry(4000, k);
    CHECK(e.replies.empty() && e.notes.empty() && e.log.size() == 1 && e.log[0].event == "holding");
    CHECK(d.retryAt() == 4000 + 2 * sync::kSecretRetryFirstMs);
    // The backoff stops growing at a minute.
    std::uint64_t at = 4000;
    for (int i = 0; i < 12; ++i) {
        at = *d.retryAt();
        d.retry(at, k);
    }
    CHECK(*d.retryAt() - at == sync::kSecretRetryMaxMs);
    // The disk fixed: the next retry writes it, the asker hears "stored", and the desk holds the value no longer.
    k.fail = false;
    e = d.retry(*d.retryAt(), k);
    CHECK(e.replies.size() == 1 && e.replies[0].stored && e.replies[0].id == 5 && e.replies[0].reason.empty());
    CHECK(e.stored.size() == 1 && e.stored[0].id == 5 && d.stateOf(5) == sync::SecretState::Stored);
    CHECK(d.holding().empty() && !d.retryAt() && !d.busy("a1") && k.files["a1/oauth-token"] == kValue);
    // In its file, its durability unconfirmed: held as well, and "stored" only once a write is durable.
    d.ask(50, asker(1, 40), "a1", kToken, "", true, 5000);
    k.unconfirmed = true;
    e = d.answer(50, sync::SecretValue(kOther), k, 5000);
    CHECK(e.replies.empty() && d.stateOf(50) == sync::SecretState::Holding && k.files["a1/oauth-token"] == kOther);
    CHECK(e.log.size() == 1 && e.log[0].event == "holding" && holds(e.log[0].detail, "the directory's sync failed"));
    k.unconfirmed = false;
    e = d.retry(5000 + sync::kSecretRetryFirstMs, k);
    CHECK(e.replies.size() == 1 && e.replies[0].stored && d.stateOf(50) == sync::SecretState::Stored);
    // Not the shape of a token: refused malformed at once, the request ended, nothing written; the person is told.
    for (const char* bad : {"two words", "two\nlines", "tab\there", "caf\xc3\xa9"}) {
        d.ask(52, asker(1, 42), "a2", kToken, "", true, 5100);
        e = d.answer(52, sync::SecretValue(bad), k, 5100);
        CHECK(e.replies.size() == 1 && !e.replies[0].stored && e.replies[0].reason == sync::reason::kMalformed);
        CHECK(e.replies[0].account == "a2" && e.replies[0].what == kToken && k.files.count("a2/oauth-token") == 0);
        CHECK(e.log.size() == 1 && e.log[0].event == "malformed" && !holds(e.log[0].line(), bad));
        CHECK(e.notes.size() == 1 && holds(e.notes[0].text, "not one token") && !d.isOpen(52));
    }
    CHECK(sync::secretShape(kToken, sync::SecretValue(kValue)).empty());
    CHECK(!sync::secretShape(kToken, sync::SecretValue("")).empty());
    CHECK(!sync::secretShape(kToken, sync::SecretValue(std::string(sync::kSecretValueBytes + 1, 'x'))).empty());
    CHECK(sync::secretShape(kToken, sync::SecretValue(std::string(sync::kSecretValueBytes, 'x'))).empty());
    // Refused before any request: busy, id 0, the reason in its lines, nothing opened.
    e = d.refuseAtOnce(asker(1, 41), "a1", kToken, "one of its asks is open already");
    CHECK(e.replies.size() == 1 && e.replies[0].id == 0 && !e.replies[0].stored &&
          e.replies[0].reason == sync::reason::kBusy && e.replies[0].asker == asker(1, 41) && !e.changed);
    CHECK(e.notes.size() == 1 && holds(e.notes[0].text, "not asked for: one of its asks is open already"));
    CHECK(e.log.size() == 1 && holds(e.log[0].line(), "busy: not asked: session 1"));
    // The deadline: not a millisecond before it, then timeout; the other request stays open.
    d.ask(6, asker(1, 5), "a9", kToken, "", true, 4000);
    d.ask(7, asker(0, 0), "a2", kToken, "", true, 4001);
    CHECK(d.deadline() == 4000 + sync::kSecretDeadlineMs);
    e = d.expire(4000 + sync::kSecretDeadlineMs - 1);
    CHECK(e.replies.empty() && d.isOpen(6));
    e = d.expire(4000 + sync::kSecretDeadlineMs);
    CHECK(e.replies.size() == 1 && e.replies[0].id == 6 && e.replies[0].reason == sync::reason::kTimeout);
    CHECK(d.isOpen(7) && d.deadline() == 4001 + sync::kSecretDeadlineMs);
    // An answer that crossed the deadline, for the newest request of its account: still stored, its asker (told
    // "timeout" already) not answered again, its session told.
    CHECK(d.stateOf(6) == sync::SecretState::Expired);
    e = d.answer(6, sync::SecretValue(kValue), k, 4000 + sync::kSecretDeadlineMs + 5);
    CHECK(e.replies.empty() && d.stateOf(6) == sync::SecretState::Stored && k.files["a9/oauth-token"] == kValue);
    CHECK(e.notes.size() == 1 && holds(e.notes[0].text, "stored after all") && e.stored.size() == 1);
    // Once a newer request for its account has been open, an expired one's answer is late.
    d.ask(8, asker(1, 6), "a9", kToken, "", true, 5000);
    d.expire(5000 + sync::kSecretDeadlineMs);
    d.ask(9, asker(1, 7), "a9", kToken, "", true, 5001 + sync::kSecretDeadlineMs);
    d.cancel(9);
    e = d.answer(8, sync::SecretValue(kOther), k, 5002 + sync::kSecretDeadlineMs);
    CHECK(e.log.size() == 1 && e.log[0].event == "stored" && k.files["a9/oauth-token"] == kOther);
    d.ask(10, asker(1, 8), "a9", kToken, "", true, 6000);
    d.expire(6000 + sync::kSecretDeadlineMs);
    d.ask(11, asker(1, 9), "a9", kToken, "", true, 6001 + sync::kSecretDeadlineMs);
    e = d.answer(10, sync::SecretValue(kValue), k, 6002 + sync::kSecretDeadlineMs);
    CHECK(e.log.size() == 1 && e.log[0].event == "late" && k.files["a9/oauth-token"] == kOther);
    d.cancel(11);
    // A value held when the server ends is lost, and said so; every open request is dropped, its asker not answered
    // (it ends with the server); an expired request is forgotten.
    d.ask(12, asker(1, 10), "a1", kToken, "", true, 7000);
    k.fail = true;
    d.answer(12, sync::SecretValue(kValue), k, 7000);
    k.fail = false;
    d.ask(13, asker(1, 11), "a2", kToken, "", true, 7000);
    d.expire(7000 + sync::kSecretDeadlineMs);
    d.ask(14, asker(0, 0), "a3", kToken, "", true, 8000 + sync::kSecretDeadlineMs);
    e = d.drop();
    CHECK(e.replies.empty() && e.log.size() == 2 && e.log[0].event == "dropped" && e.log[0].id == 14);
    CHECK(e.log[1].event == "lost" && e.log[1].id == 12 && holds(e.log[1].detail, "must be pasted again"));
    CHECK(d.stateOf(14) == sync::SecretState::Dropped && d.open().empty() && !d.deadline());
    CHECK(d.stateOf(12) == sync::SecretState::Dropped && d.holding().empty() && !d.retryAt());
    CHECK(d.stateOf(13) == sync::SecretState::Refused);
    CHECK(!d.stateOf(99));
    // It remembers the most recent ended requests only.
    for (std::uint64_t id = 100; id < 100 + sync::SecretDesk::kKeptEnded + 10; ++id) {
        d.ask(id, asker(0, 0), "a1", kToken, "", false, 5000);
    }
    CHECK(!d.stateOf(100) && d.stateOf(100 + sync::SecretDesk::kKeptEnded + 9) == sync::SecretState::Refused);
}

// ---- the window's prompt ----

void prompt() {
    const sync::SecretRequest r1{1, "a1", kToken, 1, "f", "the login expired"};
    const sync::SecretRequest r2{2, "a2", kToken, 0, "", ""};
    sync::SecretPrompt p;
    std::uint64_t id = 0;
    sync::SecretValue v;
    CHECK(p.showing() == nullptr && !p.submit(&id, &v) && !p.cancel(&id));
    p.type("nothing shows");
    CHECK(p.typed().empty());
    // The oldest shows, whatever the order received.
    CHECK(p.received({r2, r1}) && p.showing() != nullptr && p.showing()->id == 1);
    CHECK(!p.received({r1, r2}) && p.showing()->id == 1);
    // Nothing but blanks: Enter sends nothing.
    p.type("  \r\n");
    CHECK(!p.submit(&id, &v) && p.showing() != nullptr);
    // A paste with a line break and spaces at its ends: the value goes without them, once, and the field is wiped.
    p.type(" " + kValue + "\r\n");
    CHECK(p.typed().bytes() == " " + kValue + "\r\n");
    CHECK(p.submit(&id, &v) && id == 1 && v.bytes() == kValue && p.typed().empty() && p.showing() == nullptr);
    CHECK(p.answered().count(1) == 1);
    // The next state: r1, answered here, never shows again; r2 does.
    CHECK(p.received({r1, r2}) && p.showing() != nullptr && p.showing()->id == 2);
    // r2 leaves the state (answered in another window): the dialog closes, wiping what was typed.
    p.type("x");
    CHECK(p.received({r1}) && p.showing() == nullptr && p.typed().empty());
    // Escape.
    const sync::SecretRequest r3{3, "a2", kToken, 0, "", ""};
    CHECK(p.received({r1, r3}) && p.cancel(&id) && id == 3 && p.showing() == nullptr);
    // The link lost: the dialog, the field and the answers are gone; the server's next state decides again.
    p.lost();
    CHECK(p.view().empty() && p.answered().empty() && p.showing() == nullptr);
    CHECK(p.received({r1}) && p.showing() != nullptr && p.showing()->id == 1);
}

// ---- the codecs: an answer up, the requests down ----

void codecs() {
    const sync::Action a = answer(7, kValue);
    CHECK(!leaks(sync::describe(a)) && holds(sync::describe(a), "7"));
    // Format 3: the answer and the cancel both cross, the value only in the answer.
    sync::Actions mine(1);
    mine.setFormat(sync::kSecretsFormat);
    sync::Actions base(1);
    base.setFormat(sync::kSecretsFormat);
    mine.add(a);
    mine.add(cancel(8));
    const std::vector<sync::Action> got = sync::Actions::parse(mine.diff_from(base), sync::kSecretsFormat);
    CHECK(got.size() == 2);
    if (got.size() == 2) {
        CHECK(got[0].kind == sync::ActionKind::SecretAnswer && got[0].request == 7 && got[0].secret.bytes() == kValue);
        CHECK(got[1].kind == sync::ActionKind::SecretCancel && got[1].request == 8 && got[1].secret.empty());
    }
    // Format 2, which cannot carry it: a stop naming session 0, which every server refuses; the value never leaves.
    sync::Actions old(1);
    old.setFormat(2);
    sync::Actions oldBase(1);
    oldBase.setFormat(2);
    old.add(a);
    const std::string wire2 = old.diff_from(oldBase);
    CHECK(!leaks(wire2));
    const std::vector<sync::Action> got2 = sync::Actions::parse(wire2, 2);
    CHECK(got2.size() == 1 && got2[0].kind == sync::ActionKind::Stop && got2[0].session == 0 && got2[0].secret.empty());
    // The requests down: in format 3 with the state, in format 2 not at all.
    sync::Sessions s;
    s.setRequests({sync::SecretRequest{4, "a1", kToken, 1, "f", "the login expired"}});
    sync::Sessions three;
    three.apply_string(s.init_diff());
    CHECK(three.requests() == s.requests() && three == s);
    sync::Sessions s2 = s;
    s2.setFormat(2);
    sync::Sessions two;
    two.setFormat(2);
    two.apply_string(s2.init_diff());
    CHECK(two.requests().empty());
    // A format 3 state cut anywhere is refused, unchanged.
    const std::string d = s.init_diff();
    for (std::size_t n = 1; n < d.size(); ++n) {
        sync::Sessions t;
        bool threw = false;
        try {
            t.apply_string(d.substr(0, n));
        } catch (const sync::BadDiff&) {
            threw = true;
        }
        CHECK(threw && t == sync::Sessions());
    }
}

// ---- the hub over the server's store ----

void hub() {
    store::MemFs fs;
    Keeper k;
    {
        Server s(fs, k);
        std::string why;
        // Malformed asks: refused with why, no id minted.
        CHECK(!s.hub->askSecret(asker(1, 1), "nobody", kToken, "", &why) && holds(why, "no account nobody"));
        CHECK(!s.hub->askSecret(asker(1, 1), "a3", kToken, "", &why) && holds(why, "documents none"));
        CHECK(!s.hub->askSecret(asker(1, 1), "a1", "api-key", "", &why) && holds(why, "it has oauth-token"));
        CHECK(!s.hub->askSecret(asker(1, 1), "a1", kToken, "two\nlines", &why) && holds(why, "one line"));
        CHECK(!s.hub->askSecret(asker(1, 1), "a1", kToken, std::string(sync::kSecretWhyBytes + 1, 'w'), &why));
        // Names before any text: an account or a file that is not a name is refused without being repeated.
        for (const std::string& bad : {std::string("../a1"), std::string("a1\nnexus-server: forged"),
                                       std::string("a1 b"), std::string()}) {
            why.clear();
            CHECK(!s.hub->askSecret(asker(1, 1), bad, kToken, "", &why) && holds(why, "each a name"));
            CHECK(bad.empty() || !holds(why, bad));
            why.clear();
            CHECK(!s.hub->askSecret(asker(1, 1), "a1", bad, "", &why) && holds(why, "each a name"));
            CHECK(bad.empty() || !holds(why, bad));
        }
        // A reason that does not read as it is: bidirectional controls, zero-width characters, C1 controls, broken
        // UTF-8.
        for (const std::string& bad : {std::string("ok \xe2\x80\xae" "desrever"), std::string("a\xe2\x80\x8b" "b"),
                                       std::string("a\xe2\x81\xa6" "b"), std::string("a\xef\xbb\xbf" "b"),
                                       std::string("a\xc2\x85" "b"), std::string("a\x7f" "b"),
                                       std::string("a\xe2\x80\xa8" "b"), std::string("a\xff" "b"),
                                       std::string("a\xc0\xaf" "b"), std::string("a\xed\xa0\x80" "b")}) {
            why.clear();
            CHECK(!s.hub->askSecret(asker(1, 1), "a1", kToken, bad, &why) && holds(why, "bidirectional"));
        }
        CHECK(sync::isSecretWhy("the login expired \xe2\x80\x94 caf\xc3\xa9 \xf0\x9f\x94\x91"));
        CHECK(sync::isSecretWhy(std::string(sync::kSecretWhyBytes, 'w')) && sync::isSecretWhy(""));
        // A friend's AI asks only for an account its friend is linked to; nexus itself for any.
        why.clear();
        CHECK(!s.hub->askSecret(asker(1, 1), "a4", kToken, "", &why) && holds(why, "is not linked"));
        CHECK(s.store.applied(sync::kSecretIdsClient) == 0 && s.events.replies.empty());
        // An ask: id 1, durable; in the state every window receives; a line in the asker's session.
        CHECK(s.hub->askSecret(asker(1, 1), "a1", kToken, "the login expired", &why));
        CHECK(s.store.applied(sync::kSecretIdsClient) == 1);
        const std::vector<sync::SecretRequest>& open = s.hub->state().requests();
        CHECK(open.size() == 1 && open[0].id == 1 && open[0].askerName == "f" && open[0].why == "the login expired");
        CHECK(holds(s.transcript(1), "secret request 1: a1's oauth-token was asked for"));
        // Answered by a window: the login is kept, the asker hears stored, the account is signed in at once.
        CHECK(s.hub->apply(numbered(1, answer(1, kValue)), &why, "c1"));
        CHECK(k.files["a1/oauth-token"] == kValue && s.events.replies.size() == 1 && s.events.replies[0].stored);
        CHECK(s.responder.logins == std::vector<std::string>{"a1"} && s.hub->signedIn().count("a1") == 1);
        CHECK(s.hub->state().requests().empty() && holds(s.transcript(1), "a1's oauth-token was stored"));
        CHECK(s.store.applied("c1") == 1);
        // Another, left open across the restart.
        CHECK(s.hub->askSecret(asker(0, 0), "a2", kToken, "", &why));
        CHECK(s.hub->state().requests().size() == 1 && s.hub->state().requests()[0].id == 2);
    }
    {
        // A restart over the same store: ids go on from the store's counter; the open request is gone, and an answer
        // for it is dropped, nothing written.
        Server s(fs, k);
        std::string why;
        CHECK(s.hub->state().requests().empty());
        CHECK(s.hub->askSecret(asker(1, 2), "a2", kToken, "", &why));
        CHECK(s.hub->state().requests().size() == 1 && s.hub->state().requests()[0].id == 3);
        const std::size_t writes = k.writes;
        CHECK(s.hub->apply(numbered(2, answer(2, kOther)), &why, "c1"));
        CHECK(k.writes == writes && k.files.count("a2/oauth-token") == 0);
        CHECK(!s.events.log.empty() && s.events.log.back().event == "late" && !leaks(why));
        // No window that can take one: no-client at once. (nexus itself asks, for an account linked to no friend.)
        s.takers = false;
        CHECK(s.hub->askSecret(asker(0, 0), "a4", kToken, "", &why));
        CHECK(s.events.replies.back().id == 4 && s.events.replies.back().reason == sync::reason::kNoClient);
        s.takers = true;
        // The deadline by the hub's clock.
        CHECK(s.hub->secretDeadline() == s.now + sync::kSecretDeadlineMs);
        s.now += sync::kSecretDeadlineMs;
        s.hub->expireSecrets();
        CHECK(s.events.replies.back().id == 3 && s.events.replies.back().reason == sync::reason::kTimeout);
        CHECK(s.hub->state().requests().empty() && !s.hub->secretDeadline());
        // A stored file other than the login signs nothing in (none is documented yet; the login is the one).
        CHECK(s.responder.logins.empty());
    }
    {
        // One open ask per asker, and kSecretAsksPerWindow a minute: the rest are answered busy at once, with id 0, no
        // id minted and nothing opened.
        store::MemFs other;
        Keeper k2;
        Server s(other, k2);
        std::string why;
        CHECK(s.hub->askSecret(asker(1, 1), "a1", kToken, "", &why));
        CHECK(s.store.applied(sync::kSecretIdsClient) == 1 && s.hub->state().requests().size() == 1);
        CHECK(s.hub->askSecret(asker(1, 2), "a2", kToken, "", &why));
        CHECK(s.store.applied(sync::kSecretIdsClient) == 1 && s.hub->state().requests().size() == 1);
        CHECK(s.events.replies.size() == 1 && s.events.replies[0].id == 0 && s.events.replies[0].asker == asker(1, 2) &&
              s.events.replies[0].reason == sync::reason::kBusy);
        CHECK(holds(s.transcript(1), "a2's oauth-token was not asked for: one of its asks is open already"));
        // nexus itself is not held to it.
        CHECK(s.hub->askSecret(asker(0, 0), "a2", kToken, "", &why) && s.hub->state().requests().size() == 2);
        CHECK(s.hub->apply(numbered(1, cancel(1)), &why, "c1"));
        CHECK(s.hub->apply(numbered(2, cancel(2)), &why, "c1"));
        // Five a minute: four more, each cancelled, then the sixth is refused, and one a minute after the first goes.
        for (std::uint64_t n = 0; n < sync::kSecretAsksPerWindow - 1; ++n) {
            CHECK(s.hub->askSecret(asker(1, 3 + n), "a1", kToken, "", &why));
            const std::uint64_t id = s.store.applied(sync::kSecretIdsClient);
            CHECK(s.hub->secrets().isOpen(id));
            CHECK(s.hub->apply(numbered(3 + n, cancel(id)), &why, "c1"));
        }
        const std::uint64_t minted = s.store.applied(sync::kSecretIdsClient);
        s.events.replies.clear();
        CHECK(s.hub->askSecret(asker(1, 9), "a1", kToken, "", &why));
        CHECK(s.store.applied(sync::kSecretIdsClient) == minted && s.events.replies.size() == 1 &&
              s.events.replies[0].id == 0 && s.events.replies[0].reason == sync::reason::kBusy);
        CHECK(holds(s.transcript(1), "it asked 5 times in the last minute"));
        s.now += sync::kSecretAskWindowMs - 1;
        CHECK(s.hub->askSecret(asker(1, 10), "a1", kToken, "", &why) && s.store.applied(sync::kSecretIdsClient) == minted);
        s.now += 1;
        CHECK(s.hub->askSecret(asker(1, 11), "a1", kToken, "", &why));
        CHECK(s.store.applied(sync::kSecretIdsClient) == minted + 1 && s.hub->secrets().isOpen(minted + 1));
    }
    {
        // One chance: a login whose file cannot be written is held, and the account signs in only once a retry makes
        // it durable; the asker hears "stored" then, and nothing is asked again.
        store::MemFs other;
        Keeper k2;
        Server s(other, k2);
        std::string why;
        CHECK(s.hub->askSecret(asker(1, 1), "a1", kToken, "", &why));
        k2.fail = true;
        CHECK(s.hub->apply(numbered(1, answer(1, kValue)), &why, "c1"));
        CHECK(s.events.replies.empty() && s.responder.logins.empty() && s.hub->state().requests().empty());
        CHECK(s.hub->secretRetryAt() == s.now + sync::kSecretRetryFirstMs &&
              s.hub->secretWake() == s.hub->secretRetryAt());
        CHECK(holds(s.transcript(1), "nothing needs pasting again"));
        // The account's own sign-in asks nothing while its value is held.
        s.hub->reportSignedIn("a1", false);
        CHECK(s.hub->state().requests().empty() && s.store.applied(sync::kSecretIdsClient) == 1);
        // The friend whose value is held asks again: refused busy at once, with id 0, no id minted, nothing opened,
        // and the held value untouched.
        s.events.replies.clear();
        CHECK(s.hub->askSecret(asker(1, 2), "a1", kToken, "", &why));
        CHECK(s.events.replies.size() == 1 && s.events.replies[0].id == 0 && s.events.replies[0].asker == asker(1, 2) &&
              s.events.replies[0].reason == sync::reason::kBusy && !s.events.replies[0].stored);
        CHECK(s.store.applied(sync::kSecretIdsClient) == 1 && s.hub->state().requests().empty());
        CHECK(s.hub->secrets().holding().size() == 1 && s.hub->secretRetryAt() == s.now + sync::kSecretRetryFirstMs);
        s.events.replies.clear();
        k2.fail = false;
        s.now += sync::kSecretRetryFirstMs;
        s.hub->retrySecrets();
        CHECK(s.events.replies.size() == 1 && s.events.replies[0].stored && k2.files["a1/oauth-token"] == kValue);
        CHECK(s.responder.logins == std::vector<std::string>{"a1"} && s.hub->signedIn().count("a1") == 1);
        CHECK(!s.hub->secretRetryAt() && !s.hub->secretWake());
        // nexus's own ask answered with something not of a token's shape: refused malformed, and asked again (on the
        // server's next pass), with the reason in the dialog; the second paste is stored.
        CHECK(s.hub->askSecret(asker(0, 0), "a2", kToken, "", &why));
        const std::uint64_t first = s.store.applied(sync::kSecretIdsClient);
        CHECK(s.hub->apply(numbered(2, answer(first, "two words")), &why, "c1"));
        CHECK(s.events.replies.back().reason == sync::reason::kMalformed && s.hub->reaskPending());
        CHECK(s.hub->reaskSecrets() == std::optional<std::string>("a2") && !s.hub->reaskPending());
        const std::vector<sync::SecretRequest>& open = s.hub->state().requests();
        CHECK(open.size() == 1 && open[0].id == first + 1 && holds(open[0].why, "was not one token"));
        CHECK(sync::isSecretWhy(open[0].why));
        CHECK(s.hub->apply(numbered(3, answer(first + 1, kOther)), &why, "c1"));
        CHECK(s.events.replies.back().stored && k2.files["a2/oauth-token"] == kOther);
        // Asked again only while nothing for the account is open or held by then.
        CHECK(s.hub->askSecret(asker(0, 0), "a2", kToken, "", &why));
        const std::uint64_t third = s.store.applied(sync::kSecretIdsClient);
        CHECK(s.hub->apply(numbered(4, answer(third, "x y")), &why, "c1"));
        CHECK(s.hub->askSecret(asker(0, 0), "a2", kToken, "", &why));
        CHECK(!s.hub->reaskSecrets() && s.store.applied(sync::kSecretIdsClient) == third + 1);
        // A friend's AI is not asked again by nexus: it hears malformed, and asks again itself.
        CHECK(s.hub->askSecret(asker(1, 2), "a1", kToken, "", &why));
        CHECK(s.hub->apply(numbered(5, answer(third + 2, "x y")), &why, "c1"));
        CHECK(s.events.replies.back().reason == sync::reason::kMalformed && !s.hub->reaskPending());
    }
    {
        // A hub with no secret dialog wired: no asks; an answer is counted and dropped, its value nowhere.
        store::MemFs other;
        server::DurableStore store;
        std::string why;
        CHECK(store.open(other, "store", &why));
        sync::Hub h(store);
        CHECK(!h.askSecret(asker(0, 0), "a1", kToken, "", &why) && holds(why, "takes no secrets"));
        CHECK(!h.apply(numbered(1, answer(1, kValue)), &why, "c1") && holds(why, "dropped") && !leaks(why));
        CHECK(store.applied("c1") == 1);
    }
}

// ---- the account's file ----

void files() {
    store::MemFs fs;
    server::AccountFiles f(fs, "home/accounts");
    std::string why;
    CHECK(f.path("a1", kToken) == "home/accounts/a1/oauth-token");
    CHECK(f.keep("a1", kToken, sync::SecretValue(kValue), &why) == sync::Kept::Durable);
    CHECK(readFile(fs, "home/accounts/a1/oauth-token") == kValue);
    CHECK(names(fs, "home/accounts/a1") == std::vector<std::string>{"oauth-token"});
    // Replaced whole.
    CHECK(f.keep("a1", kToken, sync::SecretValue(kOther), &why) == sync::Kept::Durable);
    CHECK(readFile(fs, "home/accounts/a1/oauth-token") == kOther);
    // A temporary file a crash left goes first.
    CHECK(fs.create("home/accounts/a1/oauth-token.new").is_ok());
    const std::string torn = "torn";
    CHECK(fs.append("home/accounts/a1/oauth-token.new",
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(torn.data()), torn.size()))
              .is_ok());
    CHECK(f.keep("a1", kToken, sync::SecretValue(kValue), &why) == sync::Kept::Durable);
    CHECK(readFile(fs, "home/accounts/a1/oauth-token") == kValue);
    CHECK(names(fs, "home/accounts/a1") == std::vector<std::string>{"oauth-token"});
    // Names a path cannot hold: refused, the reason without the value.
    for (const std::string& bad : {std::string(), std::string("."), std::string(".."), std::string("a/b"),
                                   std::string("a\nb"), std::string("a\x01")}) {
        CHECK(!server::isPathPart(bad));
        why.clear();
        CHECK(f.keep(bad, kToken, sync::SecretValue(kValue), &why) == sync::Kept::Failed && !why.empty() && !leaks(why));
        why.clear();
        CHECK(f.keep("a1", bad, sync::SecretValue(kValue), &why) == sync::Kept::Failed && !why.empty() && !leaks(why));
    }
    CHECK(server::isPathPart("a1") && server::isPathPart("oauth-token") && server::isPathPart("caf\xc3\xa9"));
    // The start's sweep: every temporary file in every account's directory goes, and nothing else.
    CHECK(fs.make_dir("home/accounts/a2").is_ok());
    CHECK(fs.create("home/accounts/a1/oauth-token.new").is_ok());
    CHECK(fs.create("home/accounts/a2/api-key.new").is_ok());
    const std::vector<std::string> report = f.sweep();
    CHECK(report.size() == 2 && holds(report[0], "removed home/accounts/a1/oauth-token.new") &&
          holds(report[1], "removed home/accounts/a2/api-key.new"));
    CHECK(names(fs, "home/accounts/a1") == std::vector<std::string>{"oauth-token"});
    CHECK(names(fs, "home/accounts/a2").empty());
    CHECK(readFile(fs, "home/accounts/a1/oauth-token") == kValue);
    CHECK(f.sweep().empty());
    // No accounts directory yet: nothing to sweep, nothing said.
    store::MemFs empty;
    server::AccountFiles none(empty, "home/accounts");
    CHECK(none.sweep().empty());
}

// A crash at every call of a write: the file is the old value or the new one, never anything else, and the new one
// whenever keep() returned true (so the "stored" the asker hears after it is durable); a crash's leftovers never stop
// the next write.
void files_crash() {
    store::MemFs base;
    {
        server::AccountFiles b(base, "accounts");
        CHECK(b.keep("a1", kToken, sync::SecretValue(kOther), nullptr) == sync::Kept::Durable);
    }
    const std::string path = "accounts/a1/oauth-token";
    std::uint64_t calls = 0;
    {
        store::MemFs probe(base);
        server::AccountFiles p(probe, "accounts");
        const std::uint64_t before = probe.calls();
        CHECK(p.keep("a1", kToken, sync::SecretValue(kValue), nullptr) == sync::Kept::Durable);
        calls = probe.calls() - before;
    }
    CHECK(calls >= 5);
    for (std::uint64_t k = 1; k <= calls + 1; ++k) {
        store::MemFs fs(base);
        server::AccountFiles f(fs, "accounts");
        fs.fail_at(k);
        std::string why;
        const sync::Kept result = f.keep("a1", kToken, sync::SecretValue(kValue), &why);
        const bool ok = result == sync::Kept::Durable;
        CHECK(ok == (k > calls));
        // Unconfirmed only when the one call that failed was the directory's sync after the rename: the value was in
        // place, and the answer says no more than that.
        if (result == sync::Kept::Unconfirmed) {
            CHECK(readFile(fs, path) == kValue && holds(why, "sync of its directory failed"));
        }
        CHECK(!leaks(why));
        std::vector<store::MemFs::CrashPlan> plans{fs.plan_all(true), fs.plan_all(false)};
        for (std::uint64_t seed = 1; seed <= 16; ++seed) {
            store::Rng rng(seed * 7919 + k);
            plans.push_back(fs.plan(rng));
        }
        for (const store::MemFs::CrashPlan& plan : plans) {
            store::MemFs after(fs);
            after.crash(plan);
            const std::string kept = readFile(after, path);
            CHECK(kept == kOther || kept == kValue);
            if (ok) {
                CHECK(kept == kValue);
            }
            server::AccountFiles g(after, "accounts");
            g.sweep();
            for (const std::string& name : names(after, "accounts/a1")) {
                CHECK(name.size() < 4 || name.compare(name.size() - 4, 4, ".new") != 0);
            }
            CHECK(g.keep("a1", kToken, sync::SecretValue(kValue), nullptr) == sync::Kept::Durable);
            CHECK(readFile(after, path) == kValue);
            CHECK(names(after, "accounts/a1") == std::vector<std::string>{"oauth-token"});
        }
    }
}

// ---- the leak test ----

// A window: its state as received in its format, and its dialog.
struct Window {
    std::string name;
    sync::Sessions got;
    sync::SecretPrompt prompt;
    std::vector<std::string> received;      // every diff it was sent
};

// Every output a value could reach, gathered across the run.
struct Outputs {
    std::vector<std::string> texts;
    void add(const std::string& where, const std::string& text) { texts.push_back(where + ": " + text); }
};

// The server's step after anything: what the desk did goes out as the server sends it (each asker's driver line, the
// log's lines, the session lines), every window is sent the state in its format.
void flush(Server& s, std::vector<Window>& windows, Outputs& out) {
    // The bytes nexus-server's own SecretLines gives each asker's driver and server.log.
    server::SecretLines lines([&out](sync::SessionId, const std::string& line) { out.add("a driver's line", line); },
                              [&out](const std::string& line) { out.add("server.log", line); });
    for (const sync::SecretReply& r : s.events.replies) {
        lines.answered(r);
        out.add("an answer's reason", r.reason);
    }
    for (const sync::SecretLog& l : s.events.log) {
        lines.logged(l);
    }
    for (const sync::SecretNote& n : s.events.notes) {
        out.add("a session line", n.text);
    }
    s.events = Events();
    for (Window& w : windows) {
        sync::Sessions state = s.hub->state();
        state.setFormat(w.got.format());
        const std::string diff = state.diff_from(w.got);
        w.received.push_back(diff);
        out.add("the state sent to " + w.name, diff);
        out.add("the state sent to " + w.name + " from nothing", state.init_diff());
        w.got.apply_string(diff);
        if (w.got.format() >= sync::kSecretsFormat) {
            w.prompt.received(w.got.requests());
        }
    }
}

// The window sends its action as the link does: encoded in its format, parsed by the server and applied.
void send(Server& s, const Window& w, sync::Action action, std::uint64_t num, Outputs& out) {
    out.add("an action's description", sync::describe(action));
    sync::Actions mine(num);
    mine.setFormat(w.got.format());
    sync::Actions base(num);
    base.setFormat(w.got.format());
    mine.add(std::move(action));
    std::string wire = mine.diff_from(base);
    for (const sync::Action& a : sync::Actions::parse(wire, w.got.format())) {
        std::string why;
        s.hub->apply(a, &why, w.name);
        out.add("the hub's reason", why);
    }
    sync::wipeBytes(wire);
}

void leak() {
    store::MemFs storeFs;
    store::MemFs accountsFs;
    server::AccountFiles keeper(accountsFs, "accounts");
    Outputs out;
    std::vector<Window> windows(3);
    windows[0].name = "c1";
    windows[1].name = "c2";
    windows[2].name = "c3";
    windows[0].got.setFormat(sync::kSecretsFormat);
    windows[1].got.setFormat(sync::kSecretsFormat);
    windows[2].got.setFormat(2);   // a window from before secret requests
    {
        Server s(storeFs, keeper);
        std::string why;
        // Friend f's AI asks for a1's login; nexus itself asks for a2's.
        CHECK(s.hub->askSecret(asker(1, 11), "a1", kToken, "the login expired", &why));
        CHECK(s.hub->askSecret(asker(0, 0), "a2", kToken, "", &why));
        flush(s, windows, out);
        CHECK(windows[0].prompt.showing() != nullptr && windows[0].prompt.showing()->id == 1);
        CHECK(windows[1].prompt.showing() != nullptr && windows[1].prompt.showing()->id == 1);
        // c1 pastes the value and presses Enter; c2 has typed another into its own dialog for the same request.
        windows[1].prompt.type(kOther);
        windows[0].prompt.type(kValue + "\n");
        sync::Action a;
        a.kind = sync::ActionKind::SecretAnswer;
        CHECK(windows[0].prompt.submit(&a.request, &a.secret));
        send(s, windows[0], std::move(a), 1, out);
        flush(s, windows, out);
        CHECK(readFile(accountsFs, "accounts/a1/oauth-token") == kValue);
        // c2's request left the state: its dialog closed, what it typed wiped; it now shows a2's request.
        CHECK(windows[1].prompt.showing() != nullptr && windows[1].prompt.showing()->id == 2);
        CHECK(windows[1].prompt.typed().empty());
        // c2 answers request 1 anyway (sent before its state arrived): late, nothing written.
        send(s, windows[1], answer(1, kOther), 1, out);
        flush(s, windows, out);
        CHECK(readFile(accountsFs, "accounts/a1/oauth-token") == kValue);
        // c2 cancels a2's; then an answer for it arrives: late.
        sync::Action c;
        c.kind = sync::ActionKind::SecretCancel;
        CHECK(windows[1].prompt.cancel(&c.request) && c.request == 2);
        send(s, windows[1], std::move(c), 2, out);
        flush(s, windows, out);
        send(s, windows[0], answer(2, kOther), 2, out);
        flush(s, windows, out);
        // The old window's own answer, which its format cannot carry: refused as a stop of session 0.
        send(s, windows[2], answer(1, kOther), 1, out);
        flush(s, windows, out);
        // One left open at the end.
        CHECK(s.hub->askSecret(asker(1, 12), "a2", kToken, "once more", &why));
        flush(s, windows, out);
        windows[0].prompt.type(kOther);
        s.hub->dropSecrets();
        flush(s, windows, out);
        out.add("the asker's session", s.transcript(1));
        CHECK(holds(s.transcript(1), "was stored"));
    }
    // The old window never received a request.
    for (const std::string& diff : windows[2].received) {
        CHECK(!holds(diff, "the login expired"));
    }
    // The store's files on disk, every byte, and the account files beside the one written.
    std::vector<std::string> storeFiles = names(storeFs, "store");
    CHECK(!storeFiles.empty());
    std::string storeBytes;
    for (const std::string& n : storeFiles) {
        storeBytes += readFile(storeFs, "store/" + n);
    }
    out.add("the store's files", storeBytes);
    CHECK(names(accountsFs, "accounts/a1") == std::vector<std::string>{"oauth-token"});
    CHECK(names(accountsFs, "accounts/a2").empty());
    CHECK(readFile(accountsFs, "accounts/a1/oauth-token") == kValue);
    // The search sees plain text: the store holds the session lines, the windows' states the asker's reason.
    CHECK(holds(storeBytes, "a1's oauth-token was stored"));
    CHECK(holds(windows[0].received.front(), "the login expired"));
    // And no value is anywhere else.
    for (const std::string& text : out.texts) {
        if (leaks(text)) {
            std::fprintf(stderr, "a value reached %s\n", text.substr(0, text.find(':')).c_str());
        }
        CHECK(!leaks(text));
    }
    CHECK(out.texts.size() > 40);
}

// The search itself (tests/leak_scan.h): each way a value can be given away is found, inside other text, and text that
// holds none of them is clean.
void leak_scan() {
    const std::string v = kValue;
    const test::LeakScan scan(v, "a1");
    CHECK(scan.in("before " + v + " after"));
    CHECK(scan.in("x" + v.substr(13, 8) + "x"));                                    // a run of 8 of its bytes
    CHECK(!scan.in("x" + v.substr(13, 7) + "x"));                                   // 7 is not a run
    for (const bool upper : {false, true}) {
        CHECK(scan.in("hex: " + test::LeakScan::hex(v.substr(3, 9), upper) + "."));   // its hex, from any byte
    }
    for (std::size_t shift = 0; shift < 4; ++shift) {
        for (const bool url : {false, true}) {
            CHECK(scan.in("b64=" + test::LeakScan::base64(v.substr(shift), url)));  // its base64, at any alignment
            CHECK(scan.in("b64=" + test::LeakScan::base64(v.substr(shift, 30), url)));
        }
    }
    const std::string length = std::to_string(v.size());
    CHECK(scan.in("the account a1 was given a value of " + length + " bytes"));
    CHECK(scan.in(length + " bytes went to a1"));
    CHECK(!scan.in("the account a1 was given a value of 1" + length + " bytes"));   // another number
    CHECK(!scan.in("a value of " + length + " bytes, and " + std::string(80, '.') + " a1"));    // too far away
    CHECK(!test::LeakScan(v).in("a1 " + length));                                  // no account: no length check
    CHECK(!scan.in("secret request 1: a1's oauth-token was stored"));
    CHECK(!scan.in(test::LeakScan::hex(kOther, false) + test::LeakScan::base64(kOther, false)));
}

// ---- functional: the modes on the real filesystem (POSIX: nexus-server runs inside WSL2 on Windows) ----

#ifndef _WIN32
void files_disk() {
    const std::filesystem::path dir = std::filesystem::current_path() / "secrets_disk";
    std::filesystem::remove_all(dir);
    std::unique_ptr<store::Fs> fs = store::make_posix_fs();
    server::AccountFiles f(*fs, (dir / "accounts").string());
    std::string why;
    CHECK(f.keep("a1", kToken, sync::SecretValue(kValue), &why) == sync::Kept::Durable);
    CHECK(f.keep("a1", kToken, sync::SecretValue(kOther), &why) == sync::Kept::Durable);
    if (std::filesystem::exists(dir / "accounts" / "A1")) {
        CHECK(f.keep("A1", kToken, sync::SecretValue(kValue), &why) == sync::Kept::Failed);
        CHECK(holds(why, "spelling") && readFile(*fs, f.path("a1", kToken)) == kOther);
        const auto seen = server::inspectLogin(f.path("A1", kToken));
        CHECK(seen.file == server::LoginSeen::File::Unusable && holds(seen.problem, "spelling"));
    }
    struct stat file{};
    struct stat parent{};
    CHECK(::stat(f.path("a1", kToken).c_str(), &file) == 0 && (file.st_mode & 0777) == 0600);
    CHECK(::stat((dir / "accounts" / "a1").c_str(), &parent) == 0 && (parent.st_mode & 0777) == 0700);
    CHECK(readFile(*fs, f.path("a1", kToken)) == kOther);
    CHECK(names(*fs, (dir / "accounts" / "a1").string()) == std::vector<std::string>{"oauth-token"});

    // An account directory that is wider than 0700 is refused, and nothing is written into it.
    std::filesystem::create_directories(dir / "accounts" / "wide");
    CHECK(::chmod((dir / "accounts" / "wide").c_str(), 0755) == 0);
    const sync::Kept wideKept = f.keep("wide", kToken, sync::SecretValue(kValue), &why);
    CHECK(wideKept == sync::Kept::Failed && !leaks(why));
    CHECK(names(*fs, (dir / "accounts" / "wide").string()).empty());

    // An account directory that is a symlink is refused, even to a private directory: nothing lands at its target.
    std::filesystem::create_directories(dir / "elsewhere");
    CHECK(::chmod((dir / "elsewhere").c_str(), 0700) == 0);
    std::filesystem::create_directory_symlink(dir / "elsewhere", dir / "accounts" / "linked");
    CHECK(f.keep("linked", kToken, sync::SecretValue(kValue), &why) == sync::Kept::Failed && !leaks(why));
    CHECK(names(*fs, (dir / "elsewhere").string()).empty());

    // The temporary file is created new and never followed: write_new() on a symlink fails, and the file it points at
    // is untouched.
    const std::filesystem::path target = dir / "elsewhere" / "target";
    {
        std::FILE* t = std::fopen(target.c_str(), "w");
        CHECK(t != nullptr);
        if (t != nullptr) {
            std::fputs("untouched", t);
            std::fclose(t);
        }
    }
    const std::filesystem::path planted = dir / "accounts" / "a1" / "oauth-token.new";
    std::filesystem::create_symlink(target, planted);
    const std::string& bytes = kValue;
    const std::span<const std::uint8_t> data(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    CHECK(!fs->write_new(planted.string(), data).is_ok());
    CHECK(readFile(*fs, target.string()) == "untouched");

    // The start's sweep takes the planted name away (the link, not what it points at) and says why the two refused
    // directories were left alone.
    const std::vector<std::string> report = f.sweep();
    bool removed = false;
    std::size_t refused = 0;
    for (const std::string& line : report) {
        removed = removed || holds(line, "oauth-token.new");
        refused += holds(line, "not swept") ? 1 : 0;
    }
    CHECK(removed && refused == 2);
    CHECK(!std::filesystem::is_symlink(planted));
    CHECK(readFile(*fs, target.string()) == "untouched");

    // A link planted again is removed by keep() itself before its write, never written through.
    std::filesystem::create_symlink(target, planted);
    CHECK(f.keep("a1", kToken, sync::SecretValue(kValue), &why) == sync::Kept::Durable);
    CHECK(readFile(*fs, target.string()) == "untouched");
    CHECK(readFile(*fs, f.path("a1", kToken)) == kValue);
    CHECK(names(*fs, (dir / "accounts" / "a1").string()) == std::vector<std::string>{"oauth-token"});
    std::filesystem::remove_all(dir);
}
#endif

// ---- a login's life: how long it lasts, the notices before its end, and when a paste is asked for ----

void logins() {
    using server::LoginSeen;
    using server::kDayMs;
    const server::LoginTerm claude = server::loginTerm("claude");
    CHECK(claude.lifetimeMs == 365 * kDayMs && !claude.renewable && holds(claude.makeNew, "claude setup-token"));
    CHECK(server::loginTerm("other").lifetimeMs == 0);
    CHECK(server::dateOf(0) == "1970-01-01" && server::dateOf(365 * kDayMs) == "1971-01-01");
    const std::uint64_t t = 1000 * kDayMs;      // a made-up day the token was stored
    const LoginSeen usable{LoginSeen::File::Usable, t, ""};
    // A paste is asked for: the first login, a file not of a token's shape, one that ended, one the provider refused.
    sync::Hub::LoginAsk a = server::loginAsk("a1", "claude", LoginSeen{}, t);
    CHECK(a.ask && holds(a.text, "a1 has no Claude login yet") && sync::isSecretWhy(a.text));
    a = server::loginAsk("a1", "claude", LoginSeen{LoginSeen::File::Malformed, 0, "not one token"}, t);
    CHECK(a.ask && holds(a.text, "is not one token") && sync::isSecretWhy(a.text));
    a = server::loginAsk("a1", "claude", usable, t + 365 * kDayMs);
    CHECK(a.ask && holds(a.text, "ended on " + server::dateOf(t + 365 * kDayMs)) && holds(a.text, "cannot be renewed"));
    CHECK(sync::isSecretWhy(a.text));
    a = server::loginAsk("a1", "claude", usable, t + 10 * kDayMs);
    CHECK(a.ask && holds(a.text, "revoked") && sync::isSecretWhy(a.text));
    // Never asked when a paste would not fix it: the file as it lies; the sessions are told the fix instead.
    a = server::loginAsk("a1", "claude", LoginSeen{LoginSeen::File::Unusable, 0, "run chmod 600 it"}, t);
    CHECK(!a.ask && holds(a.text, "run chmod 600 it") && holds(a.text, "asks nothing"));
    // An account name at its longest still gives a dialog line within its bound.
    const std::string longest(64, 'n');
    for (const LoginSeen& seen : {LoginSeen{}, usable}) {
        CHECK(sync::isSecretWhy(server::loginAsk(longest, "claude", seen, t + 400 * kDayMs).text));
    }
    // Live at the start: usable and before its end.
    CHECK(server::loginLive("claude", usable, t + 364 * kDayMs));
    CHECK(!server::loginLive("claude", usable, t + 365 * kDayMs));
    CHECK(!server::loginLive("claude", LoginSeen{}, t) && server::loginLive("other", usable, t + 9999 * kDayMs));
    // The notices: 30, 7 and 1 days ahead, then the end; each once; a login stored again starts over.
    server::LoginWatch w;
    const std::vector<server::LoginWatch::Login> one{{"a1", "claude", t}, {"k", "other", t}};
    CHECK(w.check(one, t + 300 * kDayMs).empty());
    std::vector<server::LoginWatch::Notice> n = w.check(one, t + 336 * kDayMs);
    CHECK(n.size() == 1 && n[0].account == "a1" && !n[0].expired && holds(n[0].text, "in 29 days"));
    CHECK(holds(n[0].text, "ends on " + server::dateOf(t + 365 * kDayMs)) && holds(n[0].text, "cannot renew"));
    CHECK(w.check(one, t + 340 * kDayMs).empty());
    n = w.check(one, t + 359 * kDayMs);
    CHECK(n.size() == 1 && holds(n[0].text, "in 6 days"));
    n = w.check(one, t + 364 * kDayMs + 1);
    CHECK(n.size() == 1 && holds(n[0].text, "in 1 day.") && !n[0].expired);
    n = w.check(one, t + 365 * kDayMs);
    CHECK(n.size() == 1 && n[0].expired && holds(n[0].text, "ended on") && sync::isSecretWhy(n[0].text));
    CHECK(w.check(one, t + 366 * kDayMs).empty());
    const std::uint64_t t2 = t + 366 * kDayMs;
    CHECK(w.check({{"a1", "claude", t2}}, t2 + kDayMs).empty());
    // A watch that starts late (a restart) tells only the notice due then.
    server::LoginWatch late;
    n = late.check(one, t + 365 * kDayMs);
    CHECK(n.size() == 1 && n[0].expired);
    // The hub: a lost login asks in the window only when the hook says a paste can fix it, else tells the sessions.
    store::MemFs fs;
    Keeper k;
    Server s(fs, k);
    std::string why;
    CHECK(s.hub->adoptDefaults().size() == 1);   // session 1 runs on a1, f's preferred account
    s.hub->loginsWith([](const accounts::Account&) {
        return sync::Hub::LoginAsk{false, "the file is open: run chmod"};
    });
    s.hub->reportSignedIn("a1", true);
    s.hub->reportSignedIn("a1", false);
    CHECK(s.hub->state().requests().empty() && s.store.applied(sync::kSecretIdsClient) == 0);
    CHECK(holds(s.transcript(1), "the file is open: run chmod") && s.hub->signedIn().count("a1") == 0);
    CHECK(!s.events.log.empty() && s.events.log.back().event == "login");
    s.hub->loginsWith([](const accounts::Account& acct) {
        return sync::Hub::LoginAsk{true, acct.name + "'s Claude login was refused: paste a new one"};
    });
    s.hub->reportSignedIn("a1", false);
    CHECK(s.hub->state().requests().size() == 1 && s.hub->state().requests()[0].why == "a1's Claude login was refused: "
          "paste a new one" && s.hub->state().requests()[0].asker == 0);
    CHECK(s.hub->apply(numbered(1, cancel(1)), &why, "c1"));
    // Its end: signed out, and asked for, saying why; a notice ahead of it in the account's sessions.
    s.hub->reportSignedIn("a1", true);
    s.hub->tellAccount("a1", "a1's Claude login ends on 2027-11-14, in 30 days");
    CHECK(holds(s.transcript(1), "ends on 2027-11-14"));
    s.hub->needsSignIn("a1", "a1's Claude login ended on 2027-11-14");
    CHECK(s.hub->signedIn().count("a1") == 0 && s.hub->state().requests().size() == 1 &&
          s.hub->state().requests()[0].why == "a1's Claude login ended on 2027-11-14");
    // Nothing more asked while that request is open.
    s.hub->needsSignIn("a1", "again");
    s.hub->reportSignedIn("a1", false);
    CHECK(s.hub->state().requests().size() == 1);
}

#ifndef _WIN32
// Synthetic enrollment receipts under a private HOME. Exercise name, account, ancestor and marker boundaries
// through the same lookup the server uses on startup.
void enrollment_files(const std::filesystem::path& home) {
    namespace fs = std::filesystem;
    const std::string account = "A.test_1-2";
    const auto harness = home / ".nexus/server/harness";
    fs::create_directories(harness);
    for (const char* provider : {"codex", "antigravity", "grok", "muse"}) {
        const auto dir = harness / provider / account;
        fs::create_directories(dir);
        CHECK(::chmod(dir.c_str(), 0700) == 0);
        const auto receipt = dir / ".nexus-enrolled";
        const auto enrolled = [&] { return server::hasEnrollment(home.string(), provider, account); };
        const auto write = [&](const std::string& value) {
            const int fd = ::open(receipt.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            CHECK(fd >= 0 && ::write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size()));
            CHECK(::close(fd) == 0);
        };
        CHECK(!enrolled());
        write(std::string(provider) + "\n");
        CHECK(enrolled());
        CHECK(!server::hasEnrollment(home.string(), provider, "a.test_1-2"));
        CHECK(!server::hasEnrollment(home.string(), provider, "../" + account));
        CHECK(!server::hasEnrollment(home.string(), std::string(provider) + "/.", account));
        CHECK(::chmod(receipt.c_str(), 0640) == 0 && !enrolled());
        CHECK(::chmod(receipt.c_str(), 0600) == 0);
        CHECK(::chmod(dir.c_str(), 0750) == 0 && !enrolled());
        CHECK(::chmod(dir.c_str(), 0700) == 0);
        CHECK(::chmod(harness.c_str(), 0775) == 0 && !enrolled());
        CHECK(::chmod(harness.c_str(), 0755) == 0);
        CHECK(enrolled());
        fs::create_hard_link(receipt, dir / "other-name");
        CHECK(!enrolled());
        fs::remove(dir / "other-name");
        fs::rename(receipt, dir / ".NEXUS-ENROLLED");
        CHECK(!enrolled());
        fs::rename(dir / ".NEXUS-ENROLLED", receipt);
        fs::create_directory_symlink(dir, dir.parent_path() / "alias");
        CHECK(!server::hasEnrollment(home.string(), provider, "alias"));
        fs::rename(dir, dir.parent_path() / "_account");
        CHECK(server::hasEnrollment(home.string(), provider, "_account"));
        fs::rename(dir.parent_path() / "_account", dir);
        fs::rename(receipt, dir / "elsewhere");
        fs::create_symlink(dir / "elsewhere", receipt);
        CHECK(!enrolled());
        fs::remove(receipt);
        CHECK(::mkfifo(receipt.c_str(), 0600) == 0 && !enrolled());
        fs::remove(receipt);
        for (const std::string& bad : {std::string(), std::string(provider), std::string("another\n"),
                                       std::string(provider) + "\nextra", std::string(provider) + "\n\n"}) {
            write(bad);
            CHECK(!enrolled());
        }
    }
}

// Functional: real files. A login file as it lies, without its value ever in what is said of it.
void login_files() {
    using server::LoginSeen;
    const std::filesystem::path dir = std::filesystem::current_path() / "logins_disk";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    enrollment_files(dir);
    const auto claude = dir / "Personal";
    std::filesystem::create_directory(claude);
    CHECK(::chmod(claude.c_str(), 0700) == 0);
    const auto receipt = claude / "verified-claude.json";
    const auto verified = [&](const std::string& email) { return server::hasVerifiedClaude(claude.string(), email); };
    CHECK(!verified("user@example.test"));
    auto writeReceipt = [&](std::string_view body) {
        int fd = ::open(receipt.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        CHECK(fd >= 0 && ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()));
        CHECK(::close(fd) == 0);
    };
    writeReceipt(R"({"version":1,"email":"user@example.test"})");
    CHECK(!verified("user@example.test"));
    writeReceipt(R"({"version":1,"email":"User@example.test","credential":{"accessToken":"fixture","refreshToken":"fixture","expiresAt":1}})");
    CHECK(verified("user@EXAMPLE.test"));
    CHECK(!verified("user+tag@example.test") && !verified("older@example.test"));
    CHECK(!server::hasVerifiedClaude((dir / "personal").string(), "user@example.test"));
    CHECK(::chmod(receipt.c_str(), 0644) == 0 && !verified("user@example.test"));
    CHECK(::chmod(receipt.c_str(), 0600) == 0);
    std::filesystem::create_hard_link(receipt, claude / "alias");
    CHECK(!verified("user@example.test"));
    const std::string path = (dir / "oauth-token").string();
    CHECK(server::inspectLogin(path).file == LoginSeen::File::Missing);
    auto write = [&](const std::string& text, mode_t mode) {
        std::filesystem::remove(path);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
        CHECK(fd >= 0 && ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
        ::close(fd);
        ::chmod(path.c_str(), mode);
    };
    const auto before = std::chrono::system_clock::now();
    write(kValue + "\n", 0600);
    LoginSeen seen = server::inspectLogin(path);
    const std::uint64_t ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        before.time_since_epoch()).count());
    CHECK(seen.file == LoginSeen::File::Usable && seen.storedAtMs + 2000 >= ms && seen.storedAtMs <= ms + 60000);
    write(kValue, 0644);
    seen = server::inspectLogin(path);
    CHECK(seen.file == LoginSeen::File::Unusable && holds(seen.problem, "chmod 600") && !leaks(seen.problem));
    write("two words " + kValue, 0600);
    seen = server::inspectLogin(path);
    CHECK(seen.file == LoginSeen::File::Malformed && !leaks(seen.problem));
    std::filesystem::remove(path);
    write(kValue, 0600);
    std::filesystem::rename(path, dir / "elsewhere");
    std::filesystem::create_symlink(dir / "elsewhere", path);
    seen = server::inspectLogin(path);
    CHECK(seen.file == LoginSeen::File::Unusable && holds(seen.problem, "symbolic link") && !leaks(seen.problem));
    std::filesystem::remove_all(dir);
}
#endif

struct Group {
    const char* name;
    void (*run)();
    bool unit;
};

const Group kGroups[] = {
    {"desk", desk, true},
    {"prompt", prompt, true},
    {"codecs", codecs, true},
    {"hub", hub, true},
    {"files", files, true},
    {"files_crash", files_crash, true},
    {"leak", leak, true},
    {"leak_scan", leak_scan, true},
    {"login_life", logins, true},
#ifndef _WIN32
    {"files_disk", files_disk, false},
    {"login_files", login_files, false},
#endif
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : kGroups) {
        if (argc > 1 ? std::strcmp(argv[1], g.name) != 0 : !g.unit) {
            continue;
        }
        g.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "secrets_test: no group %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "secrets_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("secrets_test: ok\n");
    return 0;
}
