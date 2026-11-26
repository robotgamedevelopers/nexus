// Unit tests of nexus itself across versions: this build's real wire code (sync::Sessions, sync::Actions, the Hello of
// sync/formats.h) against a later version's generated code (tests/wire/next/NexusNext.schema, wire_next.cpp) and an
// earlier one's, format 4 (tests/wire/prev/NexusPrev.schema, wire_prev.cpp), each writing what the other reads, both
// ways. In memory only: no time, no sockets, no files.
//
//   nexus_versions_test           runs every group
//   nexus_versions_test <group>   runs one:
//     sessions   a later server's state diff applied by this build's window: the added kind shows as a notice, the
//                added activity as busy, the added field is skipped, each counted (Sessions::unknown), and nothing is
//                refused; this build's server's diffs (a first one and one that grows an open entry) read whole by a
//                later window, nothing unknown
//     actions    a later window's actions read by this build's server: the added kind reads as unknown (which the
//                server refuses one by one, never the link), the added field is skipped, both counted, the rest as
//                written, the secret's bytes included; this build's window's actions read whole by a later server
//     hello      each version's Hello read by the other: the added field is skipped and counted; the decision
//                (sync::refusalOfServer, sync::refusalOfWindow) refuses only when the other's proto is below this
//                end's min_peer
//     signin     format 4 (tests/wire/prev: nexus before the in-app sign-in) against this build's 5, both ways: the
//                link of 4 from the Hellos, on which no window acts on a sign-in (so the server asks for a paste);
//                the flows' field skipped and counted by a format 4 window; a format 5 window's sign-in actions read
//                as None by a format 4 server, and written on the link of 4 as a stop naming session 0; this build's
//                server reading a sign-in kind on the link of 4 as unknown, alone; a format 4 server's state whole

#include "versions.h"

#include "sync/actions.h"
#include "sync/formats.h"
#include "sync/sessions.h"
#include "sync/wire.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace nexus::sync;
using wiretest::Bytes;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

std::string text(const Bytes& b) {
    return std::string(b.begin(), b.end());
}

Bytes bytes(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    return c - 'a' + 10;
}

std::string hex(const Bytes& b) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t c : b) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

Bytes unhex(const std::string& s) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(hexDigit(s[i]) * 16 + hexDigit(s[i + 1])));
    }
    return out;
}

EntryRef entry(nexus::EntryKind kind, const std::string& t, bool closed = true) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, t, closed});
}

SessionRef session(SessionId id, const std::string& aiName, Activity activity, std::vector<EntryRef> entries,
                   std::vector<std::string> queue = {}) {
    auto v = std::make_shared<SessionView>();
    v->id = id;
    v->info.aiName = aiName;
    v->info.harness = "claude";
    v->info.instance = "studio";
    v->activity = activity;
    v->entries = std::move(entries);
    v->queue = std::move(queue);
    return v;
}

void test_sessions() {
    using Names = std::vector<std::string>;

    // A later server's state, applied by this build's window.
    Sessions window;
    bool threw = false;
    try {
        window.apply_string(text(wiretest::nextSessions()));
    } catch (const BadDiff& e) {
        std::fprintf(stderr, "a later server's diff was refused: %s\n", e.what());
        threw = true;
    }
    CHECK(!threw);
    CHECK(window.unknown() == 3);       // the title, the Image kind, the Waiting activity
    CHECK(window.sessions().size() == 2);
    if (window.sessions().size() == 2) {
        const SessionView& a = *window.sessions()[0];
        CHECK(a.id == 7 && a.info.aiName == "rowan" && a.info.instance == "studio");
        CHECK(a.activity == Activity::Busy);
        CHECK(a.entries.size() == 3);
        if (a.entries.size() == 3) {
            CHECK(a.entries[0]->kind == nexus::EntryKind::Person && a.entries[0]->text == "hi");
            CHECK(a.entries[1]->kind == nexus::EntryKind::Notice && a.entries[1]->text == "a picture");
            CHECK(a.entries[2]->kind == nexus::EntryKind::Tool && a.entries[2]->text == "ls");
        }
        CHECK((a.queue == Names{"later"}));
        const SessionView& b = *window.sessions()[1];
        CHECK(b.id == 8 && b.info.aiName == "stella");
        CHECK(b.activity == Activity::Busy);    // Waiting, which this build does not know, shows as busy
        CHECK(b.entries.size() == 1);
    }
    // The sign-in flows, which both know: read whole, the held one's URL as its bytes.
    CHECK(window.offers().size() == 2);
    if (window.offers().size() == 2) {
        CHECK(window.offers()[0].id == 4 && window.offers()[0].opener == "w1" && window.offers()[0].host == "studio");
        CHECK(window.offers()[0].url.bytes() == "https://claude.com/cai/oauth/authorize?state=test-state");
        CHECK(window.offers()[1].id == 5 && window.offers()[1].opener == "*" && window.offers()[1].url.empty());
    }

    // This build's server's diffs, read by a later window: a first diff, then one that grows an open entry.
    const Sessions empty;
    const Sessions first({session(7, "rowan", Activity::Busy,
                                  {entry(nexus::EntryKind::Person, "hi"), entry(nexus::EntryKind::AI, "hel", false)},
                                  {"later"})});
    const std::string firstDiff = first.diff_from(empty);
    const wiretest::Read r1 = wiretest::nextReadSessions(bytes(firstDiff));
    CHECK(r1.loaded && !r1.damaged && !r1.refused && r1.unknown == 0);
    CHECK(r1.aiName == "rowan" && r1.instance == "studio" && !r1.titlePresent);
    CHECK((r1.kinds == Names{"Person", "AI"}));
    CHECK((r1.queue == Names{"later"}));
    const Sessions grown({session(7, "rowan", Activity::Idle,
                                  {entry(nexus::EntryKind::Person, "hi"), entry(nexus::EntryKind::AI, "hello")})});
    const wiretest::Read r2 = wiretest::nextReadSessions(bytes(grown.diff_from(first)));
    CHECK(r2.loaded && !r2.damaged && !r2.refused && r2.unknown == 0);
    CHECK((r2.kinds == Names{"AI"}));       // only the entry that grew
    // And this build reads its own the same, nothing unknown.
    Sessions own;
    own.apply_string(firstDiff);
    CHECK(own == first && own.unknown() == 0);
}

void test_actions() {
    using Names = std::vector<std::string>;

    // A later window's actions, read by this build's server.
    int unknown = -1;
    std::vector<Action> read;
    bool threw = false;
    try {
        read = Actions::parse(text(wiretest::nextActions()), kStateFormat, &unknown);
    } catch (const BadDiff& e) {
        std::fprintf(stderr, "a later window's actions were refused: %s\n", e.what());
        threw = true;
    }
    CHECK(!threw);
    CHECK(unknown == 2);    // the Rename kind and the note field
    CHECK(read.size() == 4);
    if (read.size() == 4) {
        CHECK(read[0].num == 5 && read[0].kind == ActionKind::Send && read[0].session == 7 && read[0].text == "hi");
        // Unknown, and nothing of it is kept: what its fields mean is the later version's.
        CHECK(read[1].num == 6 && read[1].kind == ActionKind::Unknown && read[1].session == 0 && read[1].text.empty());
        CHECK(read[2].num == 7 && read[2].kind == ActionKind::SecretAnswer && read[2].request == 3);
        CHECK(read[2].secret.bytes() == "test-value");
        CHECK(read[3].num == 8 && read[3].kind == ActionKind::Stop && read[3].session == 7);
    }
    // The server's state takes them as one unbroken run.
    Actions server(5);
    server.apply_string(text(wiretest::nextActions()));
    CHECK(server.next() == 9);

    // This build's window's actions, read by a later server.
    Actions window(3);
    Action send;
    send.kind = ActionKind::Send;
    send.session = 7;
    send.text = "hi";
    window.add(send);
    Action create;
    create.kind = ActionKind::Create;
    create.session = 7;
    create.aiName = "stella";
    window.add(create);
    Action answer;
    answer.kind = ActionKind::SecretAnswer;
    answer.request = 4;
    answer.secret = SecretValue("test-value");
    window.add(answer);
    const wiretest::ReadActions r = wiretest::nextReadActions(bytes(window.diff_from(Actions(3))));
    CHECK(r.loaded && !r.damaged && !r.refused && r.unknown == 0);
    CHECK(r.start == 3);
    CHECK((r.kinds == Names{"Send", "Create", "SecretAnswer"}));
    CHECK((r.texts == Names{"hi", "", ""}));
    CHECK((r.ais == Names{"", "stella", ""}));
    CHECK((r.sessions == std::vector<std::uint64_t>{7, 7, 0}));
    CHECK((r.requests == std::vector<std::uint64_t>{0, 0, 4}));
    CHECK((r.secrets == Names{"", "", "test-value"}));
}

void test_hello() {
    // A later end's Hello, read by this build.
    Hello later;
    std::string why;
    int unknown = -1;
    CHECK(readHello(hex(wiretest::nextHello(6, 2)), &later, &why, &unknown));
    CHECK(later == (Hello{6, 2}));
    CHECK(unknown > 0);     // the build field, skipped and counted (0 when a later Hello leaves it out)
    // It works with this build either way round: this build's proto is not below its min_peer, nor its below ours.
    CHECK(refusalOfServer(later).empty());
    CHECK(refusalOfWindow(later).empty());
    // A later end that no longer works with this build's proto is refused, in plain words, whichever end it is.
    Hello strict;
    CHECK(readHello(hex(wiretest::nextHello(9, kStateFormat + 1)), &strict, &why, &unknown));
    const std::string server = refusalOfServer(strict);
    CHECK(server.find("too old") != std::string::npos);
    const std::string window = refusalOfWindow(strict);
    CHECK(window.find("too old") != std::string::npos);
    CHECK(server != window);

    // This build's Hello, read by a later end: whole, nothing unknown, and no build (this build never writes one).
    const wiretest::ReadHello ours = wiretest::nextReadHello(unhex(helloText(kHello)));
    CHECK(ours.loaded && ours.unknown == 0 && !ours.buildPresent);
    CHECK(ours.proto == kStateFormat && ours.minPeer == kMinPeer);
}

// Format 4 (nexus before the in-app sign-in, tests/wire/prev) and this build's format 5, both ways.
void test_signin() {
    using Names = std::vector<std::string>;
    using Ids = std::vector<std::uint64_t>;

    // The Hello says it all: a format 4 end works with this build, the link is of 4, and a window on it does not act
    // on a sign-in, whichever end is the older.
    Hello prev;
    std::string why;
    int unknown = -1;
    CHECK(readHello(hex(wiretest::prevHello()), &prev, &why, &unknown));
    CHECK(prev == (Hello{kTablesFormat, 1}) && unknown == 0);
    CHECK(refusalOfWindow(prev).empty() && refusalOfServer(prev).empty());
    const std::uint32_t link = linkFormat(prev.proto, kStateFormat);
    CHECK(link == kTablesFormat && link == linkFormat(kStateFormat, prev.proto));
    CHECK(!takesSignIn(link) && takesSignIn(linkFormat(kStateFormat, kStateFormat)));

    // A format 5 server's state with flows, read by a format 4 window: the flows' field skipped and counted, the
    // sessions and the requests whole. (A server never writes it so to a window of 4; this is what such a window would
    // make of it.)
    Sessions server({session(7, "rowan", Activity::Busy, {entry(nexus::EntryKind::Person, "hi")})});
    server.setRequests({SecretRequest{3, "a1", "oauth-token", 0, "nexus", "a1 has no login"}});
    server.setOffers({SignInOffer{4, "a1", "claude", "w1", "studio", SecretValue("https://claude.com/?state=test")},
                      SignInOffer{5, "a2", "claude", "*", "studio", SecretValue()}});
    const wiretest::Read skipped = wiretest::prevReadSessions(bytes(server.init_diff()));
    CHECK(skipped.loaded && !skipped.damaged && !skipped.refused);
    CHECK(skipped.unknown == 2);    // the skipped field counts once for each flow it holds (measured: 1 for one flow)
    CHECK(skipped.sessions == 1 && skipped.aiName == "rowan" && (skipped.kinds == Names{"Person"}));
    CHECK((skipped.requests == Ids{3}));
    // An older reader which predates the roster still loads the transcript and requests when
    // the new server includes nested named account choices, labels and configured email.
    Sessions withRoster = server;
    withRoster.setAIs({{"rowan", {{"work", "Writing", true, "writing@example.test"}, {"home", ""}}}});
    const auto earlier = wiretest::prevReadSessions(bytes(withRoster.init_diff()));
    CHECK(earlier.loaded && !earlier.damaged && !earlier.refused);
    CHECK(earlier.sessions == skipped.sessions && earlier.aiName == skipped.aiName);
    CHECK(earlier.requests == skipped.requests && earlier.kinds == skipped.kinds);
    // What this server sends on the link of 4: no flows at all, so nothing unknown; the requests as before.
    Sessions onLink = server;
    onLink.setFormat(link);
    const wiretest::Read four = wiretest::prevReadSessions(bytes(onLink.init_diff()));
    CHECK(four.loaded && !four.damaged && !four.refused && four.unknown == 0);
    CHECK(four.sessions == 1 && (four.requests == Ids{3}));

    // A format 5 window's actions, sign-in kinds among them, read by a format 4 server: those kinds read None (which
    // such a server refuses one by one, never the link), counted, the rest as written, the numbers unbroken.
    Actions window(5);
    Action send;
    send.kind = ActionKind::Send;
    send.session = 7;
    send.text = "hi";
    window.add(send);
    Action claim;
    claim.kind = ActionKind::SignInClaim;
    claim.request = 4;
    claim.text = "w1";
    window.add(claim);
    Action callback = claim;
    callback.kind = ActionKind::SignInCallback;
    callback.secret = SecretValue("code=test-code&state=test");
    window.add(callback);
    Action answer;
    answer.kind = ActionKind::SecretAnswer;
    answer.request = 3;
    answer.secret = SecretValue("test-value");
    window.add(answer);
    const std::string fromFive = window.diff_from(Actions(5));
    const wiretest::ReadActions none = wiretest::prevReadActions(bytes(fromFive));
    CHECK(none.loaded && !none.damaged && !none.refused && none.unknown == 2);
    CHECK(none.start == 5 && (none.kinds == Names{"Send", "None", "None", "SecretAnswer"}));
    CHECK(none.secrets.size() == 4 && none.secrets[3] == "test-value");
    // What this window sends on the link of 4: each sign-in action a stop naming session 0 and nothing else, which
    // every server refuses; nothing of the sign-in leaves.
    Actions windowOnLink = window;
    windowOnLink.setFormat(link);
    const wiretest::ReadActions stops = wiretest::prevReadActions(bytes(windowOnLink.diff_from(Actions(5))));
    CHECK(stops.loaded && !stops.damaged && !stops.refused && stops.unknown == 0);
    CHECK((stops.kinds == Names{"Send", "Stop", "Stop", "SecretAnswer"}));
    CHECK((stops.sessions == Ids{7, 0, 0, 0}) && (stops.texts == Names{"hi", "", "", ""}));
    CHECK((stops.requests == Ids{0, 0, 0, 3}) && (stops.secrets == Names{"", "", "", "test-value"}));
    // This build's server on the link of 4: a format 4 window's actions whole; and should a sign-in kind come on it,
    // unknown, nothing of it kept, the rest read.
    int skippedUp = -1;
    const std::vector<Action> fromFour = Actions::parse(text(wiretest::prevActions()), link, &skippedUp);
    CHECK(skippedUp == 0 && fromFour.size() == 2);
    if (fromFour.size() == 2) {
        CHECK(fromFour[0].kind == ActionKind::Send && fromFour[0].text == "hi");
        CHECK(fromFour[1].kind == ActionKind::SecretAnswer && fromFour[1].secret.bytes() == "test-value");
    }
    const std::vector<Action> unknownUp = Actions::parse(fromFive, link);
    CHECK(unknownUp.size() == 4);
    if (unknownUp.size() == 4) {
        CHECK(unknownUp[0].kind == ActionKind::Send && unknownUp[3].kind == ActionKind::SecretAnswer);
        for (const std::size_t i : {std::size_t(1), std::size_t(2)}) {
            CHECK(unknownUp[i].kind == ActionKind::Unknown && unknownUp[i].text.empty() && unknownUp[i].request == 0 &&
                  unknownUp[i].secret.empty());
        }
    }

    // The reverse: a format 4 server's state, read by this build's window on the link of 4: whole, nothing unknown,
    // no flows.
    Sessions newer;
    newer.setFormat(link);
    newer.apply_string(text(wiretest::prevSessions()));
    CHECK(newer.unknown() == 0 && newer.offers().empty() && newer.sessions().size() == 1 && newer.requests().size() == 1);
}

void test_bus() {
    const std::string payload = "bo\nfirst line\nsecond line";
    const Sessions complete({session(7, "ada", Activity::Idle, {entry(nexus::EntryKind::Bus, payload)})});
    // A wire diff can carry only the suffix of an entry already held. Its kind and sender survive
    // when that suffix contains more lines of the body.
    const Sessions partial({session(7, "ada", Activity::Idle, {entry(nexus::EntryKind::Bus, "bo\nfirst", false)})});
    Sessions receiver;
    receiver.apply_string(partial.init_diff());
    receiver.apply_string(complete.diff_from(partial));
    CHECK(receiver == complete);

    const auto snapshot = bytes(complete.init_diff());
    const std::vector<std::string> texts{payload};
    const auto previous = wiretest::prevReadSessions(snapshot);
    CHECK(previous.loaded && !previous.damaged && !previous.refused && previous.unknown == 1);
    CHECK(previous.kinds == std::vector<std::string>{"None"} && previous.texts == texts);
    const auto next = wiretest::nextReadSessions(snapshot);
    CHECK(next.loaded && !next.damaged && !next.refused && next.unknown == 0);
    CHECK(next.kinds == std::vector<std::string>{"Bus"} && next.texts == texts);

    auto withSenders = [&](std::vector<std::string> senders) {
        auto view = std::make_shared<SessionView>(*complete.sessions().front());
        view->busSenders = std::move(senders);
        return Sessions({view});
    };
    const std::vector<std::string> senders{"bo", "cy"};
    const Sessions known = withSenders(senders);
    const Sessions none = withSenders({});
    CHECK(receiver.find(7) && !receiver.find(7)->busSenders);
    receiver.apply_string(known.diff_from(complete));
    CHECK(receiver.find(7) && receiver.find(7)->busSenders == senders);
    receiver.apply_string(none.diff_from(known));
    CHECK(receiver.find(7) && receiver.find(7)->busSenders && receiver.find(7)->busSenders->empty());
    receiver.apply_string(complete.diff_from(none));
    CHECK(receiver.find(7) && !receiver.find(7)->busSenders);
    const auto earlier = wiretest::prevReadSessions(bytes(known.init_diff()));
    CHECK(earlier.loaded && !earlier.damaged && !earlier.refused);
    CHECK(earlier.unknown > previous.unknown && earlier.kinds == previous.kinds && earlier.texts == texts);

    for (std::uint32_t format : {1u, 2u, 3u}) {
        Sessions server = known;
        server.setFormat(format);
        Sessions withoutSenders = complete;
        withoutSenders.setFormat(format);
        CHECK(server.init_diff() == withoutSenders.init_diff());
        Sessions window;
        window.setFormat(format);
        window.apply_string(server.init_diff());
        const auto* visible = window.find(7);
        CHECK(visible && !visible->busSenders && visible->entries.size() == 1);
        if (visible && visible->entries.size() == 1) {
            CHECK(visible->entries[0]->kind == nexus::EntryKind::Notice && visible->entries[0]->text == payload);
        }
    }
}

struct Group {
    const char* name;
    void (*run)();
};

const Group groups[] = {
    {"sessions", test_sessions},
    {"bus", test_bus},
    {"actions", test_actions},
    {"hello", test_hello},
    {"signin", test_signin},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : groups) {
        if (argc < 2 || std::strcmp(argv[1], g.name) == 0) {
            g.run();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "nexus_versions_test: no group named %s\n", argv[1]);
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "nexus_versions_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("nexus_versions_test: ok\n");
    return 0;
}
