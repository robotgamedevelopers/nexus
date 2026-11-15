// Tests of in-app sign-in (docs/secrets.md "Sign-in", src/sync/signin.h): the URL and callback parsing, the server's
// desk, the window's opener, the one-shot gate, the token scanner, the codecs (the three actions up, the offers down
// and their absence for older peers), the hub over the server's store with the runner's hooks recorded, and the leak
// test: a made-up URL, callback and token through a whole sign-in, then every output searched for their bytes.
//
// Every group is a unit group: logic only, no time (the clock is an argument), no sockets, no processes, no files (the
// store is a MemFs, the account's file a map). Every URL, code, state and token is made up here: never a real one, and
// no real sign-in page is ever loaded.
//
//   signin_test            runs every group
//   signin_test <group>    runs one

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "leak_scan.h"
#include "server/durable.h"
#include "server/secret_lines.h"
#include "store/fs.h"
#include "sync/actions.h"
#include "sync/formats.h"
#include "sync/hub.h"
#include "sync/secrets.h"
#include "sync/sessions.h"
#include "sync/signin.h"
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

// The made-up values. The state and the code are long and odd enough that no other text holds them by chance; the
// URL's own words (its host, its path) are not secret, so the scans look for its state, and the code, and the token.
const std::string kState = "fakeStQm8vTrXw4kGdHbN2pL7cJ9sZ1aE6";
const std::string kCode = "fakeCoPw3kGdHbX5rT8yU2iO4mN7qW1eR9";
const std::string kOtherState = "fakeStOtherZ3xY6vB8nM1lK4jH7gF2dS";
const std::string kUrl = "https://claude.com/cai/oauth/authorize?code=true&client_id=fake-client&response_type=code"
                         "&redirect_uri=http%3A%2F%2Flocalhost%3A54545%2Fcallback&scope=user%3Ainference&state=" + kState;
const std::string kToken = "sk-ant-oat01-fakeTkZq8wX3cV6bN9mA2sD5fG7hJ1kL4pO0iU" + std::string(48, 'q') + "AA";
const std::string kApiKey = "sk-ant-api03-fakeKyZq8wX3cV6bN9mA2sD5fG7hJ1kL4pO0iU" + std::string(48, 'k') + "AA";
const std::string kCallback = "code=" + kCode + "&state=" + kState;

bool holds(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool leaks(const std::string& text) {
    static const std::vector<test::LeakScan> scans{
        test::LeakScan(kState), test::LeakScan(kCode), test::LeakScan(kToken, "a1"), test::LeakScan(kApiKey, "a1"),
        test::LeakScan(kToken, "a2"), test::LeakScan(kOtherState)};
    return std::any_of(scans.begin(), scans.end(), [&](const test::LeakScan& s) { return s.in(text); });
}

sync::SecretValue secret(const std::string& s) {
    return sync::SecretValue(s);
}

// ---------------------------------------------------------------------------------------------------------------------

void urls() {
    sync::SignInTarget t;
    CHECK(sync::parseSignInUrl("claude", kUrl, &t));
    CHECK(t.port == 54545 && t.path == "/callback" && t.state == kState);
    CHECK(sync::parseSignInUrl("claude", "https://claude.ai/a?redirect_uri=http://127.0.0.1:9/cb/x&state=s", &t));
    CHECK(t.port == 9 && t.path == "/cb/x" && t.state == "s");
    CHECK(sync::parseSignInUrl("claude", "https://platform.claude.com/a?redirect_uri=http%3A%2F%2Flocalhost%3A80&state=s",
                               &t) && t.path == "/");
    // Refused: not https, no state, a redirect elsewhere or with no port, port 0 or past 65535, a space, a redirect
    // that carries its own query.
    for (const std::string& bad : {
             std::string("http://claude.com/a?redirect_uri=http://localhost:9/cb&state=s"),
             std::string("https://claude.com/a?redirect_uri=http://localhost:9/cb"),
             std::string("https://claude.com/a?redirect_uri=http://evil.example:9/cb&state=s"),
             std::string("https://claude.com/a?redirect_uri=https://localhost:9/cb&state=s"),
             std::string("https://claude.com/a?redirect_uri=http://localhost/cb&state=s"),
             std::string("https://claude.com/a?redirect_uri=http://localhost:0/cb&state=s"),
             std::string("https://claude.com/a?redirect_uri=http://localhost:65536/cb&state=s"),
             std::string("https://claude.com/a b?redirect_uri=http://localhost:9/cb&state=s"),
             std::string("https://claude.com/a?redirect_uri=http%3A%2F%2Flocalhost%3A9%2Fcb%3Fx%3D1&state=s"),
             std::string("https://claude.com/a?redirect_uri=http://localhost.evil.example:9/cb&state=s"),
             std::string("https://claude.com/a?redirect_uri=http://localhost:9/cb&state=s\r\nX: y"),
             std::string("https://claude.com/a?redirect_uri=http://localhost:9/cb&state=s\x01"),
             std::string("https://claude.com/a"), std::string()}) {
        CHECK(!sync::parseSignInUrl("claude", bad, &t));
    }
    // The hosts: the one list per harness, compared byte for byte, at the server and in the window alike. Refused: a
    // look-alike in each form (userinfo before the host, a trailing dot, upper case, punycode, a host that only starts
    // or ends with the right name, a port), another host, and every host for a provider with no sign-in.
    const std::string rest = "/cai/oauth/authorize?redirect_uri=http://localhost:9/cb&state=s";
    for (const std::string& host : {"claude.com", "platform.claude.com", "claude.ai"}) {
        CHECK(sync::signInHostAllowed("claude", host) && sync::parseSignInUrl("claude", "https://" + host + rest, &t));
        CHECK(!sync::signInHostAllowed("codex", host) && !sync::parseSignInUrl("codex", "https://" + host + rest, &t));
    }
    for (const std::string& host : {"claude.com@evil.example", "evil.example@claude.com", "user:pw@claude.com",
                                    "claude.com.", "CLAUDE.COM", "Claude.com", "xn--claude-9ta.com", "xn--clude-dva.com",
                                    "claude.com.evil.example", "claude.community", "evilclaude.com", "claude.com:443",
                                    "claude.com:8443", "claude.co", "anthropic.com", "", "claude.com\\@evil.example",
                                    "claude.com%2eevil.example"}) {
        CHECK(!sync::parseSignInUrl("claude", "https://" + host + rest, &t));
    }
    CHECK(sync::urlHost("https://a.example:1/x?y") == "a.example:1" && sync::urlHost("https://u@h?q") == "u@h" &&
          sync::urlHost("https://h#f") == "h" && sync::urlHost("http://h/").empty());
    // The compare: equal bytes only, lengths aside.
    CHECK(sync::sameBytes(kState, kState) && !sync::sameBytes(kState, kOtherState) &&
          !sync::sameBytes(kState, kState + "x") && !sync::sameBytes("", "a") && sync::sameBytes("", ""));
    CHECK(sync::isQueryBytes(kCallback) && !sync::isQueryBytes(kCallback + "\r\nX: y") &&
          !sync::isQueryBytes("code=a b") && !sync::isQueryBytes("code=a\tb") && !sync::isQueryBytes("code=\x7f"));
    CHECK(sync::queryValue("a=1&b=x%20y+z&c", "b") == "x y z");
    CHECK(sync::queryValue("a=1&b=2", "c").empty() && sync::queryValue("ab=1&b=2", "b") == "2");
    CHECK(sync::queryValue("a=%zz%4", "a") == "%zz%4");

    CHECK(sync::isClaimToken("abc123") && !sync::isClaimToken("") && !sync::isClaimToken("a-b") &&
          !sync::isClaimToken(std::string(sync::kSignInTokenBytes + 1, 'a')));
    CHECK(sync::isSubscriptionToken(secret(kToken)) && !sync::isSubscriptionToken(secret(kApiKey)));
    CHECK(!sync::isSubscriptionToken(secret("sk-ant-oat01-short")) &&
          !sync::isSubscriptionToken(secret(kToken + " trailing")));
    // Its exact shape: "sk-ant-oat01-" and token bytes, 40 to 256 bytes in all.
    CHECK(!sync::isSubscriptionToken(secret("sk-ant-oat02-" + kToken.substr(13))) &&
          !sync::isSubscriptionToken(secret("sk-ant-oatx1-" + kToken.substr(13))) &&
          !sync::isSubscriptionToken(secret(kToken + std::string(256, 'q'))) &&
          sync::isSubscriptionToken(secret("sk-ant-oat01-" + std::string(243, 'q'))) &&
          !sync::isSubscriptionToken(secret("sk-ant-oat01-" + std::string(244, 'q'))) &&
          sync::isSubscriptionToken(secret("sk-ant-oat01-" + std::string(27, 'q'))) &&
          !sync::isSubscriptionToken(secret("sk-ant-oat01-" + std::string(26, 'q'))));

    // Same machine: a loopback server, or the server's host name the window's own (first label, case aside).
    CHECK(sync::sameMachine("localhost", "", "") && sync::sameMachine("me@127.0.0.1", "", ""));
    CHECK(sync::sameMachine("[::1]:22", "", "") && sync::sameMachine("localhost:2222", "", ""));
    CHECK(sync::sameMachine("studio", "Studio.local", "studio") && sync::sameMachine("x", "studio", "STUDIO.lan"));
    CHECK(!sync::sameMachine("studio", "studio.local", "laptop") && !sync::sameMachine("studio", "", ""));
}

// ---------------------------------------------------------------------------------------------------------------------

void desk() {
    sync::SignInDesk d;
    d.setHost("studio.local");
    // Needed: offered, no process yet; a second need for the account changes nothing.
    sync::SignInEffects e = d.need(1, "a1", "claude");
    CHECK(e.changed && e.start.empty() && d.live("a1") && d.stateOf(1) == sync::SignInState::Needed);
    CHECK(e.log.size() == 1 && holds(e.log[0], "sign-in 1: a1: needed") && e.notes.size() == 1);
    e = d.need(2, "a1", "claude");
    CHECK(!e.changed && e.log.empty() && !d.stateOf(2));
    std::vector<sync::SignInOffer> o = d.offers();
    CHECK(o.size() == 1 && o[0].id == 1 && o[0].account == "a1" && o[0].opener.empty() && o[0].host == "studio.local" &&
          o[0].url.empty());
    // A claim with a bad token is late; a claim of a needed flow starts its process, due in 10 minutes.
    e = d.claim(1, "a-b", 1000);
    CHECK(!e.changed && e.start.empty() && d.stateOf(1) == sync::SignInState::Needed);
    e = d.claim(1, "w1", 1000);
    CHECK(e.changed && e.start.size() == 1 && e.start[0].id == 1 && e.start[0].account == "a1");
    CHECK(d.stateOf(1) == sync::SignInState::Started && d.deadline() == 1000 + sync::kSignInDeadlineMs);
    // A second window's claim, and the same window's again, start nothing.
    e = d.claim(1, "w2", 1001);
    CHECK(e.start.empty() && !e.changed && d.offers()[0].opener == "w1");
    e = d.claim(1, "w1", 1001);
    CHECK(e.start.empty() && !e.changed);
    // Opened before its URL: late. A URL for another flow id: late.
    CHECK(d.opened(1, "w1").log.size() == 1 && d.stateOf(1) == sync::SignInState::Started);
    CHECK(!d.url("a1", 9, secret(kUrl)).changed && d.stateOf(1) == sync::SignInState::Started);
    // The URL: offered to its holder (offers() carries it; the hub strips it for every other link).
    e = d.url("a1", 1, secret(kUrl));
    CHECK(e.changed && d.stateOf(1) == sync::SignInState::UrlSent);
    o = d.offers();
    CHECK(o.size() == 1 && o[0].url.bytes() == kUrl && o[0].opener == "w1");
    // Opened by another window: late; by its holder: waiting.
    CHECK(!d.opened(1, "w2").changed && d.stateOf(1) == sync::SignInState::UrlSent);
    CHECK(d.opened(1, "w1").changed && d.stateOf(1) == sync::SignInState::Waiting);
    // Callbacks: from another window, with another state, with neither code nor error: dropped, nothing replayed.
    CHECK(d.callback(1, "w2", secret(kCallback)).replays.empty());
    CHECK(d.callback(1, "w1", secret("code=" + kCode + "&state=" + kOtherState)).replays.empty());
    CHECK(d.callback(1, "w1", secret("state=" + kState)).replays.empty());
    CHECK(d.stateOf(1) == sync::SignInState::Waiting);
    // The matching one: replayed once, to the URL's own redirect.
    e = d.callback(1, "w1", secret(kCallback));
    CHECK(e.replays.size() == 1 && e.replays[0].query.bytes() == kCallback && e.replays[0].target.port == 54545 &&
          e.replays[0].target.path == "/callback" && d.stateOf(1) == sync::SignInState::CallbackForwarded);
    CHECK(d.callback(1, "w1", secret(kCallback)).replays.empty());
    // The token: captured, the flow ends, its process is ended, nothing is needed again.
    sync::SignInEffects p;
    CHECK(d.printed("a1", &p) == 1 && d.stateOf(1) == sync::SignInState::Captured && !d.live("a1"));
    CHECK(p.stop == std::vector<std::string>{"a1"} && p.needAgain.empty() && p.changed && d.offers().empty());
    CHECK(!d.deadline());
    // A login printed with no flow running is late: kept (the caller stores it), and a needed flow for the account,
    // made when the last ended, is dropped; nothing is needed again.
    sync::SignInEffects none;
    CHECK(d.printed("a1", &none) == 0 && none.log.size() == 1 && holds(none.log[0], "kept") && none.stop.empty());
    d.need(20, "a1", "claude");
    sync::SignInEffects late;
    CHECK(d.printed("a1", &late) == 0 && d.stateOf(20) == sync::SignInState::Dropped && !d.live("a1") &&
          late.needAgain.empty() && late.stop.empty() && late.changed);

    // Endings: a declined page (the replayed callback carried an error) is cancelled; any other exit failed; each
    // leaves the account needing a fresh flow.
    d.need(3, "a2", "claude");
    d.claim(3, "w1", 2000);
    d.url("a2", 3, secret(kUrl));
    d.opened(3, "w1");
    d.callback(3, "w1", secret("error=access_denied&state=" + kState));
    e = d.exited("a2", 3, "it exited 1");
    CHECK(d.stateOf(3) == sync::SignInState::Cancelled && e.needAgain == std::vector<std::string>{"a2"} &&
          e.stop.empty() && e.notes.size() == 1 && holds(e.notes[0].text, "declined"));
    d.need(4, "a2", "claude");
    d.claim(4, "w1", 2000);
    e = d.exited("a2", 4, "it exited 1");
    CHECK(d.stateOf(4) == sync::SignInState::Failed && e.needAgain == std::vector<std::string>{"a2"});
    // A URL that is not a sign-in URL with a local redirect fails the flow, its process ended.
    d.need(5, "a2", "claude");
    d.claim(5, "w1", 2000);
    e = d.url("a2", 5, secret("https://claude.com/?redirect_uri=http://evil.example:1/x&state=s"));
    CHECK(d.stateOf(5) == sync::SignInState::Failed && e.stop == std::vector<std::string>{"a2"});
    // A URL to a host not on the provider's list fails the flow the same way, with a plain line.
    d.need(10, "a2", "claude");
    d.claim(10, "w1", 2000);
    e = d.url("a2", 10, secret("https://claude.com.evil.example/oauth/authorize?redirect_uri=http://localhost:9/cb"
                               "&state=" + kState));
    CHECK(d.stateOf(10) == sync::SignInState::Failed && e.stop == std::vector<std::string>{"a2"} &&
          std::any_of(e.log.begin(), e.log.end(), [](const std::string& l) { return holds(l, "refused"); }));
    for (const std::string& l : e.log) {
        CHECK(!leaks(l) && !holds(l, "evil"));
    }
    // A callback with a control byte or a space in it is dropped: it is replayed as it came, in a request line.
    d.need(11, "a2", "claude");
    d.claim(11, "w1", 2000);
    d.url("a2", 11, secret(kUrl));
    d.opened(11, "w1");
    CHECK(d.callback(11, "w1", secret(kCallback + "\r\nX-Smuggled: 1")).replays.empty());
    CHECK(d.callback(11, "w1", secret(kCallback + "&x=a b")).replays.empty());
    CHECK(d.callback(11, "w1", secret(kCallback + "&x=\x01")).replays.empty());
    CHECK(d.stateOf(11) == sync::SignInState::Waiting && d.callback(11, "w1", secret(kCallback)).replays.size() == 1);
    d.signedIn("a2");
    // The deadline: past it the flow expires and its process is ended; not before.
    d.need(6, "a2", "claude");
    d.claim(6, "w1", 3000);
    CHECK(d.expire(3000 + sync::kSignInDeadlineMs - 1).log.empty() && d.stateOf(6) == sync::SignInState::Started);
    e = d.expire(3000 + sync::kSignInDeadlineMs);
    CHECK(d.stateOf(6) == sync::SignInState::Expired && e.stop == std::vector<std::string>{"a2"} &&
          e.needAgain == std::vector<std::string>{"a2"});
    // A window's link ends: its flows are let go of; another window takes it over, and nothing starts.
    d.need(7, "a2", "claude");
    d.claim(7, "w1", 4000);
    d.url("a2", 7, secret(kUrl));
    e = d.windowGone("w1");
    CHECK(e.changed && d.offers()[0].opener.empty() && d.offers()[0].url.empty() && e.stop.empty());
    e = d.claim(7, "w2", 4001);
    CHECK(e.start.empty() && e.changed && d.offers()[0].opener == "w2" && d.offers()[0].url.bytes() == kUrl);
    // Signed in otherwise: dropped, its process ended, nothing needed. The server ends: every flow dropped.
    e = d.signedIn("a2");
    CHECK(d.stateOf(7) == sync::SignInState::Dropped && e.stop == std::vector<std::string>{"a2"} && e.needAgain.empty());
    d.need(8, "a1", "claude");
    d.need(9, "a2", "claude");
    d.claim(9, "w1", 5000);
    e = d.drop();
    CHECK(d.offers().empty() && e.stop == std::vector<std::string>{"a2"} && e.needAgain.empty());
    CHECK(d.stateOf(8) == sync::SignInState::Dropped && d.stateOf(9) == sync::SignInState::Dropped);
    // No line of any of it carries the URL, the state, the code.
    for (const std::string& l : e.log) {
        CHECK(!leaks(l));
    }
}

// ---------------------------------------------------------------------------------------------------------------------

bool sameNever(const sync::SignInOffer&, const void*) {
    return false;
}

bool sameAlways(const sync::SignInOffer&, const void*) {
    return true;
}

void opener() {
    sync::SignInOpener w("w1");
    sync::SignInOffer a{1, "a1", "claude", "", "studio", {}};
    sync::SignInOffer b{2, "a2", "claude", "", "studio", {}};
    w.received({a, b});
    // Nothing until the person shows the overlay; then one claim, of the oldest offer, and the show is spent.
    CHECK(w.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None);
    w.shown();
    sync::SignInOpener::Step s = w.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Claim && s.id == 1 && !w.armed());
    CHECK(w.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None);
    // Shown again: the next offer not claimed before; never the same one twice.
    w.shown();
    s = w.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Claim && s.id == 2);
    w.shown();
    CHECK(w.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None && w.armed());
    // Its URL arrives: opened once, with the gate first (another machine).
    a.opener = "w1";
    a.url = secret(kUrl);
    b.opener = "w9";
    w.received({a, b});
    s = w.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Open && s.id == 1 && s.url.bytes() == kUrl && s.gate &&
          s.target.port == 54545 && s.target.state == kState && w.gate() == 1);
    CHECK(w.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None);
    // The flow leaves the state: the gate closes. Reconnected, nothing is claimed or opened twice.
    w.received({b});
    CHECK(w.gate() == 0);
    w.lost();
    CHECK(w.view().empty() && !w.armed() && w.claimed().size() == 2 && w.opened().size() == 1);
    w.received({a});
    w.shown();
    CHECK(w.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None);
    // On the server's machine: opened with no gate.
    sync::SignInOpener local("w2");
    sync::SignInOffer c{3, "a1", "claude", "w2", "studio", secret(kUrl)};
    local.received({c});
    s = local.next(sameAlways, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Open && !s.gate && local.gate() == 0);
    // An offer whose URL another window holds, or a URL that does not parse, is never opened.
    sync::SignInOpener other("w3");
    other.received({c});
    CHECK(other.next(sameAlways, nullptr).kind == sync::SignInOpener::Step::Kind::None);
    // A URL that does not parse, or to a host not on the provider's list (the window's own check, as the server's):
    // refused, once, opened never, no gate; the window says so.
    sync::SignInOpener bad("w4");
    bad.received({sync::SignInOffer{4, "a1", "claude", "w4", "studio", secret("https://x.example/?state=s")},
                  sync::SignInOffer{5, "a2", "claude", "w4", "studio",
                                    secret("https://claude.com.evil.example/cai/oauth/authorize?redirect_uri="
                                           "http://localhost:9/cb&state=" + kState)}});
    s = bad.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Refused && s.id == 4 && s.url.empty() && !s.gate &&
          bad.gate() == 0);
    s = bad.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Refused && s.id == 5 && s.url.empty() && bad.gate() == 0);
    CHECK(bad.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None);
    // Two flows held by one window on another machine: the second page waits, unopened, while the first's gate waits
    // for its callback; it opens, with the gate, once that gate closes (models/SignIn.tla GateForEveryPage). A page
    // that needs no gate opens at once.
    sync::SignInOpener two("w5");
    const std::string url2 = "https://claude.com/cai/oauth/authorize?redirect_uri=http://localhost:54546/callback"
                             "&state=" + kOtherState;
    const sync::SignInOffer f1{6, "a1", "claude", "w5", "studio", secret(kUrl)};
    const sync::SignInOffer f2{7, "a2", "claude", "w5", "studio", secret(url2)};
    two.received({f1, f2});
    s = two.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Open && s.id == 6 && s.gate && two.gate() == 6);
    CHECK(two.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None && two.opened().count(7) == 0);
    CHECK(two.next(sameAlways, nullptr).kind == sync::SignInOpener::Step::Kind::Open && two.gate() == 6);
    sync::SignInOpener queued("w5");
    queued.received({f1, f2});
    CHECK(queued.next(sameNever, nullptr).id == 6 && queued.next(sameNever, nullptr).kind ==
          sync::SignInOpener::Step::Kind::None);
    queued.gateClosed();
    s = queued.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Open && s.id == 7 && s.gate && s.target.port == 54546 &&
          s.target.state == kOtherState && queued.gate() == 7);
    // The first flow leaving the state closes its gate too, and the second opens.
    sync::SignInOpener left("w5");
    left.received({f1, f2});
    CHECK(left.next(sameNever, nullptr).id == 6);
    left.received({f2});
    s = left.next(sameNever, nullptr);
    CHECK(s.kind == sync::SignInOpener::Step::Kind::Open && s.id == 7 && left.gate() == 7);
    // A window with no claim token takes no part.
    sync::SignInOpener anonymous;
    anonymous.received({a});
    anonymous.shown();
    CHECK(anonymous.next(sameNever, nullptr).kind == sync::SignInOpener::Step::Kind::None);
}

// ---------------------------------------------------------------------------------------------------------------------

std::string get(const std::string& target) {
    return "GET " + target + " HTTP/1.1\r\nHost: localhost:54545\r\nAccept: */*\r\n\r\n";
}

void gate() {
    // The gate's connections: each has its time from its accept, whatever it sends (the time an argument here); one
    // past the cap is refused, and a slot freed is taken again.
    {
        sync::GateSlots slots;
        std::size_t slot = 99;
        for (std::size_t i = 0; i < sync::GateSlots::kConnections; ++i) {
            CHECK(slots.accept(1000 + i, &slot) && slot == i);
        }
        CHECK(!slots.accept(1100, &slot) && slots.used() == sync::GateSlots::kConnections);
        CHECK(slots.expire(1000 + sync::GateSlots::kConnectionMs - 1).empty());
        CHECK((slots.expire(1000 + sync::GateSlots::kConnectionMs) == std::vector<std::size_t>{0}));
        CHECK(slots.accept(20000, &slot) && slot == 0);
        slots.release(3);
        CHECK(slots.used() == sync::GateSlots::kConnections - 1 && slots.accept(20001, &slot) && slot == 3);
        // A slot trickling bytes is no different: its deadline is its accept's.
        CHECK(slots.expire(1001 + sync::GateSlots::kConnectionMs) == std::vector<std::size_t>{1});
        // At 30000: every slot but 3 (accepted at 20001) has run out.
        CHECK(slots.expire(30000).size() == sync::GateSlots::kConnections - 2 && slots.used() == 1);
        slots.release(99);
        CHECK(slots.used() == 1);
    }
    sync::SignInTarget t;
    CHECK(sync::parseSignInUrl("claude", kUrl, &t));
    // A callback whose query holds a space or a control byte is refused (it would be replayed as it came): a space
    // inside the request target, a tab, a DEL.
    {
        sync::CallbackGate c(t);
        sync::SecretValue q;
        std::string response;
        for (const std::string& smuggled : {"GET /callback?" + kCallback + "&x=a b HTTP/1.1\r\n\r\n",
                                            "GET /callback?" + kCallback + "&x=a\tb HTTP/1.1\r\n\r\n",
                                            "GET /callback?" + kCallback + "&x=\x7f HTTP/1.1\r\n\r\n"}) {
            CHECK(c.take(smuggled, &q, &response) == sync::CallbackGate::Verdict::Refuse && q.empty() && !c.taken());
        }
    }
    sync::CallbackGate g(t);
    sync::SecretValue q;
    std::string response;
    // Partial bytes: more.
    CHECK(g.take("GET /callback?code=", &q, &response) == sync::CallbackGate::Verdict::More && q.empty());
    // Anything not its callback is "not found" and changes nothing: another path, another state, no code or error, a
    // POST, a favicon, junk, a request past the cap with no end.
    for (const std::string& stray : {get("/favicon.ico"), get("/callback?code=" + kCode + "&state=" + kOtherState),
                                     get("/callback?state=" + kState), get("/callbackx?" + kCallback),
                                     get("/?" + kCallback), "POST /callback?" + kCallback + " HTTP/1.1\r\n\r\n",
                                     std::string("\r\n\r\n"), std::string("GET\r\n\r\n"),
                                     std::string(sync::CallbackGate::kRequestBytes + 10, 'x')}) {
        response.clear();
        CHECK(g.take(stray, &q, &response) == sync::CallbackGate::Verdict::Refuse && q.empty());
        CHECK(holds(response, "404 Not Found") && !g.taken());
    }
    // The one: taken, answered plainly, its query handed over.
    CHECK(g.take(get("/callback?" + kCallback), &q, &response) == sync::CallbackGate::Verdict::Take);
    CHECK(q.bytes() == kCallback && holds(response, "200 OK") && holds(response, "signed in, you can close this tab"));
    CHECK(!leaks(response) && g.taken());
    // After the one, every request is refused, the same one too.
    sync::SecretValue again;
    CHECK(g.take(get("/callback?" + kCallback), &again, &response) == sync::CallbackGate::Verdict::Refuse &&
          again.empty());
    // An error (the person declined) is taken too, and said so.
    sync::CallbackGate d(t);
    CHECK(d.take(get("/callback?error=access_denied&state=" + kState), &q, &response) ==
          sync::CallbackGate::Verdict::Take && holds(response, "not finished"));
}

// ---------------------------------------------------------------------------------------------------------------------

void scanner() {
    // The token as `claude setup-token` prints it, among colours, cursor moves, a title and the words around it, fed a
    // byte at a time and cut at every place.
    const std::string screen = "\x1b[2J\x1b[HOpening browser...\r\n\x1b]0;claude\x07\x1b[32m\xe2\x9c\x93 Long-lived "
                               "token created\x1b[0m\r\n\r\nYour OAuth token (valid for 1 year):\r\n\r\n\x1b[1m" +
                               kToken + "\x1b[22m\r\n\r\nStore this token securely.\r\n";
    for (std::size_t cut = 0; cut <= screen.size(); cut += 7) {
        sync::TokenScanner s;
        const bool first = s.feed(std::string_view(screen).substr(0, cut));
        const bool second = s.feed(std::string_view(screen).substr(cut));
        CHECK(!first || cut >= screen.find(kToken) + kToken.size());
        CHECK(second && s.found());
        CHECK(s.take().bytes() == kToken);
    }
    sync::TokenScanner bytewise;
    for (char c : screen) {
        bytewise.feed(std::string_view(&c, 1));
    }
    CHECK(bytewise.found() && bytewise.take().bytes() == kToken);
    // Not ended yet: not found until a byte that cannot be in a token follows.
    sync::TokenScanner open;
    CHECK(!open.feed("token: " + kToken) && open.feed("\r\n") && open.take().bytes() == kToken);
    // A line that names the prefix alone, and a run of token bytes longer than any login, are not a token.
    sync::TokenScanner words;
    CHECK(!words.feed("tokens start with sk-ant- and go on\r\n" + std::string(9000, 'a') + "\n"));
    CHECK(!words.found());
    // An escape sequence inside the token's bytes is skipped (a terminal's colour change mid-line).
    sync::TokenScanner coloured;
    CHECK(coloured.feed(kToken.substr(0, 20) + "\x1b[0m" + kToken.substr(20) + "\n") &&
          coloured.take().bytes() == kToken);
    // Every escape form, at every place around and inside the token, the whole fed split in two at every byte: a
    // charset select (ESC and an intermediate byte, then its final byte), two-byte escapes, a CSI with parameters, and
    // the strings (OSC to BEL or ST, DCS, SOS, PM, APC to ST). None of its bytes is taken as the terminal's text.
    const std::vector<std::string> escapes{
        "\x1b(B", "\x1b)0", "\x1b(0", "\x1b#8", "\x1b%G", "\x1b 7", "\x1b" "7", "\x1b" "8", "\x1b=", "\x1b>", "\x1bM",
        "\x1b" "c", "\x1b[38;5;12m", "\x1b[?25l", "\x1b]0;title sk-ant-oat01-" + std::string(40, 'z') + "\x07",
        "\x1b]8;;https://x.example\x1b\\", "\x1bPq#0;2;0;0;0sk-ant-oat01-" + std::string(40, 'd') + "\x1b\\",
        "\x1bXsos\x1b\\", "\x1b^pm\x1b\\", "\x1b_apc sk-ant-oat01-" + std::string(40, 'a') + "\x1b\\"};
    const std::vector<std::size_t> places{0, 3, 7, 13, 20, kToken.size() - 1, kToken.size()};
    for (const std::string& esc : escapes) {
        for (std::size_t at : places) {
            const std::string text = "token:\r\n" + kToken.substr(0, at) + esc + kToken.substr(at) + "\r\n";
            for (std::size_t cut = 0; cut <= text.size(); ++cut) {
                sync::TokenScanner s;
                s.feed(std::string_view(text).substr(0, cut));
                s.feed(std::string_view(text).substr(cut));
                const bool ok = s.found() && s.take().bytes() == kToken;
                CHECK(ok);
                if (!ok) {
                    std::fprintf(stderr, "  escape %zu bytes at %zu, cut at %zu\n", esc.size(), at, cut);
                }
            }
        }
    }
    // A first candidate that is not a subscription's login (an API key, a prefix too short to be one, a login of
    // another kind) is passed over, and the scan goes on to the login.
    for (const std::string& first : {kApiKey, std::string("sk-ant-oat01-short"),
                                     "sk-ant-oat02-" + std::string(60, 'w'), "sk-ant-oat01-" + std::string(300, 'w')}) {
        const std::string text = first + "\r\nYour OAuth token:\r\n" + kToken + "\r\n";
        for (std::size_t cut = 0; cut <= text.size(); cut += 5) {
            sync::TokenScanner s;
            s.feed(std::string_view(text).substr(0, cut));
            s.feed(std::string_view(text).substr(cut));
            CHECK(s.found() && s.take().bytes() == kToken);
        }
    }

    // The URL as the terminal prints it after its first try to open a browser: found whole, at any cut, among other
    // links; a link that is not a sign-in URL with a local redirect is passed over.
    const std::string printed = "\x1b[2mBrowser didn't open? Use the url below to sign in (c to copy)\x1b[0m\r\n"
                                "https://docs.example/help hhttps://x\r\n\x1b[4m" + kUrl + "\x1b[24m\r\n";
    for (std::size_t cut = 0; cut <= printed.size(); cut += 11) {
        sync::UrlScanner u("claude");
        u.feed(std::string_view(printed).substr(0, cut));
        CHECK(u.feed(std::string_view(printed).substr(cut)) && u.take().bytes() == kUrl);
    }
    sync::UrlScanner unended("claude");
    CHECK(!unended.feed(kUrl) && unended.feed(" ") && unended.take().bytes() == kUrl);
    sync::UrlScanner other("claude");
    CHECK(!other.feed("see https://docs.example/a?state=s and https://claude.com/?redirect_uri=http://evil.example:1/c"
                      "&state=s\n") && !other.found());
    // A sign-in shaped URL to a host not on the list is passed over, and the scan goes on to the one that is.
    sync::UrlScanner lookalike("claude");
    CHECK(!lookalike.feed("https://claude.com.evil.example/cai/oauth/authorize?redirect_uri=http://localhost:9/cb"
                          "&state=" + kState + "\r\n") && !lookalike.found());
    CHECK(lookalike.feed(kUrl + "\r\n") && lookalike.take().bytes() == kUrl);
    // A charset select before the URL leaves none of its bytes in it.
    sync::UrlScanner selected("claude");
    CHECK(selected.feed("\x1b(B" + kUrl + "\x1b(0\r\n") && selected.take().bytes() == kUrl);
}

// ---------------------------------------------------------------------------------------------------------------------

void codecs() {
    // The three actions on a link of format 5, schema's tables: the claim token, the request, the query (as bytes).
    sync::Action claim;
    claim.kind = sync::ActionKind::SignInClaim;
    claim.request = 4;
    claim.text = "w1";
    sync::Action opened = claim;
    opened.kind = sync::ActionKind::SignInOpened;
    sync::Action cb = claim;
    cb.kind = sync::ActionKind::SignInCallback;
    cb.secret = secret(kCallback);
    sync::Actions up;
    up.setFormat(sync::kSignInFormat);
    up.add(claim);
    up.add(opened);
    up.add(cb);
    const std::vector<sync::Action> got = sync::Actions::parse(up.init_diff(), sync::kSignInFormat);
    CHECK(got.size() == 3 && got[0].kind == sync::ActionKind::SignInClaim && got[0].request == 4 &&
          got[0].text == "w1");
    CHECK(got[1].kind == sync::ActionKind::SignInOpened && got[2].kind == sync::ActionKind::SignInCallback &&
          got[2].secret.bytes() == kCallback);
    CHECK(holds(sync::describe(got[2]), "sign-in 4") && !leaks(sync::describe(got[2])));
    // Every link below 5 carries none of them: each goes as a stop naming session 0 and nothing else, which every
    // server refuses, and the query never leaves; the numbers stay unbroken.
    for (const std::uint32_t format : {std::uint32_t(2), sync::kSecretsFormat, sync::kTablesFormat}) {
        sync::Actions older;
        older.setFormat(format);
        older.add(cb);
        older.add(claim);
        const std::string wire = older.init_diff();
        CHECK(!leaks(wire) && !holds(wire, "w1"));
        const std::vector<sync::Action> back = sync::Actions::parse(wire, format);
        CHECK(back.size() == 2 && back[0].kind == sync::ActionKind::Stop && back[0].session == 0 &&
              back[1].kind == sync::ActionKind::Stop && back[1].session == 0 && back[0].secret.empty() &&
              back[1].text.empty());
    }
    // A server reading a link of 4 takes none of them even if they come: unknown, nothing of them kept.
    const std::vector<sync::Action> asFour = sync::Actions::parse(up.init_diff(), sync::kTablesFormat);
    CHECK(asFour.size() == 3);
    for (const sync::Action& a : asFour) {
        CHECK(a.kind == sync::ActionKind::Unknown && a.text.empty() && a.request == 0 && a.secret.empty());
    }

    // The offers, schema's SessionsDiff.signins, on a link of 5 or later: read back whole by this build, the URL's
    // bytes as they are; a state with none is byte for byte format 4's; and a link of 4 or older carries none (a window
    // before sign-ins: the server asks it for a paste instead, Hub::signInWindowsChanged).
    sync::Sessions s;
    s.setFormat(sync::kSignInFormat);
    s.setRequests({sync::SecretRequest{7, "a3", "api-key", 0, "nexus", "why"}});
    const std::string before = s.init_diff();
    sync::Sessions four = s;
    four.setFormat(sync::kTablesFormat);
    CHECK(four.init_diff() == before);
    s.setOffers({sync::SignInOffer{1, "a1", "claude", "w1", "studio", secret(kUrl)},
                 sync::SignInOffer{2, "a2", "claude", "", "studio", {}}});
    const std::string after = s.init_diff();
    CHECK(after != before && holds(after, kState));     // the URL rides in the holder's state alone
    sync::Sessions back;
    back.setFormat(sync::kSignInFormat);
    back.apply_string(after);
    CHECK(back == s && back.offers().size() == 2 && back.offers()[0].url.bytes() == kUrl &&
          back.offers()[1].opener.empty() && back.requests().size() == 1);
    for (const std::uint32_t format : {std::uint32_t(2), sync::kSecretsFormat, sync::kTablesFormat}) {
        sync::Sessions toOld = s;
        toOld.setFormat(format);
        const std::string wire = toOld.init_diff();
        CHECK(!leaks(wire) && !holds(wire, "a1") && !holds(wire, "w1"));
        sync::Sessions read;
        read.setFormat(format);
        read.apply_string(wire);
        CHECK(read.offers().empty());
        if (format == sync::kTablesFormat) {
            CHECK(wire == before);
        }
    }
    // A window reading a link of 4 takes no offer even if one comes.
    sync::Sessions asFourWindow;
    asFourWindow.setFormat(sync::kTablesFormat);
    asFourWindow.apply_string(after);
    CHECK(asFourWindow.offers().empty() && asFourWindow.requests().size() == 1);
    // Offers gone: the next diff says so.
    sync::Sessions gone = s;
    gone.setOffers({});
    sync::Sessions next = back;
    next.apply_string(gone.diff_from(back));
    CHECK(next.offers().empty() && next == gone);
    // Out of order or twice: refused whole.
    sync::Sessions twice;
    twice.setFormat(sync::kSignInFormat);
    twice.setOffers({sync::SignInOffer{2, "a1", "claude", "", "", {}}, sync::SignInOffer{2, "a2", "claude", "", "", {}}});
    sync::Sessions reader;
    reader.setFormat(sync::kSignInFormat);
    bool threw = false;
    try {
        reader.apply_string(twice.init_diff());
    } catch (const sync::BadDiff&) {
        threw = true;
    }
    CHECK(threw && reader.offers().empty());
    // No sign-in in a state file: the URL is for one window alone.
    const std::string file = sync::stateFile(s);
    CHECK(!leaks(file) && !holds(file, "w1"));
    sync::Sessions fromFile;
    std::string why;
    CHECK(sync::readStateFile(file, &fromFile, &why) && fromFile.offers().empty() && fromFile.requests().size() == 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// The hub over the server's store, with the runner's hooks recorded.

struct Keeper : sync::SecretKeeper {
    std::map<std::string, std::string> files;
    bool fail = false;
    sync::Kept keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
                    std::string* why) override {
        if (fail) {
            *why = "the disk is full";
            return sync::Kept::Failed;
        }
        files[account + "/" + what] = value.bytes();
        return sync::Kept::Durable;
    }
};

struct Events : sync::SecretEvents {
    std::vector<std::string> lines;
    std::vector<sync::SecretReply> replies;
    void answered(const sync::SecretReply& reply) override { replies.push_back(reply); }
    void logged(const sync::SecretLog& line) override { lines.push_back(line.line()); }
    void signInLogged(const std::string& line) override { lines.push_back(line); }
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

struct Runner {
    std::vector<std::pair<std::uint64_t, std::string>> started;
    std::vector<std::string> stopped;
    std::vector<std::string> replayed;      // the queries, to check where they went and that they went once
};

// nexus-server as the tests need it: friend f's session 1 on a1; a1 and a2 (claude), a3 (a provider with no in-app
// sign-in); a2 signed in by its file at the start (the probe); sign-in wired to the recorded runner.
struct Server {
    server::DurableStore store;
    std::unique_ptr<sync::Hub> hub;
    Events events;
    Responder responder;
    Runner runner;
    std::uint64_t now = 1000;

    Server(store::MemFs& fs, sync::SecretKeeper& keeper, std::function<bool()> signInTakers = {}) {
        std::string why;
        CHECK(store.open(fs, "store", &why));
        hub = std::make_unique<sync::Hub>(store, sync::Limits{}, &responder);
        sync::Action ensure;
        ensure.kind = sync::ActionKind::Ensure;
        ensure.friendName = "f";
        hub->apply(ensure, &why);
        accounts::Registry r;
        r.accounts = {accounts::Account{"a1", "claude"}, accounts::Account{"a2", "claude"},
                      accounts::Account{"a3", "other"}};
        r.friends = {accounts::Friend{"f", {"a1", "a2", "a3"}, "a1"}};
        hub->probeSignInWith([](const accounts::Account& a) { return a.name == "a2"; });
        CHECK(hub->useAccounts(r, nullptr).empty());
        hub->adoptDefaults();
        hub->secretsWith(&keeper, &events, [] { return true; }, [this] { return now; });
        sync::SignInHooks hooks;
        hooks.start = [this](std::uint64_t id, const std::string& account) { runner.started.emplace_back(id, account); };
        hooks.stop = [this](const std::string& account) { runner.stopped.push_back(account); };
        hooks.replay = [this](std::uint64_t, const std::string& account, const sync::SignInTarget& t,
                              const sync::SecretValue& q) {
            runner.replayed.push_back(account + " " + std::to_string(t.port) + t.path + " " + q.bytes());
        };
        hooks.takers = std::move(signInTakers);
        hub->signInWith(std::move(hooks), "studio.local");
    }

    std::string transcript(sync::SessionId id) const {
        std::string out;
        if (const sync::StoredSession* s = store.find(id)) {
            for (const sync::EntryRef& e : s->transcript) {
                out += e->text + "\n";
            }
        }
        return out;
    }

    std::uint64_t num = 0;
    bool act(sync::ActionKind kind, std::uint64_t request, const std::string& token, const std::string& value = {}) {
        sync::Action a;
        a.num = ++num;
        a.kind = kind;
        a.request = request;
        a.text = token;
        a.secret = secret(value);
        std::string why;
        return hub->apply(a, &why, "c1");
    }
};

void hub() {
    store::MemFs fs;
    Keeper k;
    {
        Server s(fs, k);
        // At the start: a1 has no login, so a flow (id 1, durable) is offered; a2 is signed in; a3 signs in by paste.
        const std::vector<sync::SignInOffer>& o = s.hub->state().offers();
        CHECK(o.size() == 1 && o[0].id == 1 && o[0].account == "a1" && o[0].host == "studio.local");
        CHECK(s.store.applied(sync::kSignInIdsClient) == 1 && s.runner.started.empty());
        CHECK(s.hub->state().requests().empty() && holds(s.transcript(1), "a1 needs signing in"));
        // A driver's no-credential, an AI's ask: the flow is live, nothing more is minted.
        s.hub->reportSignedIn("a1", false);
        std::string said;
        CHECK(s.hub->requestSignIn("a1", &said) && holds(said, "needs sign-in") && !leaks(said));
        CHECK(s.store.applied(sync::kSignInIdsClient) == 1);
        CHECK(s.hub->requestSignIn("a2", &said) && said == "signed in");
        CHECK(!s.hub->requestSignIn("a3", &said) && holds(said, "does not sign in"));
        CHECK(!s.hub->requestSignIn("nobody", &said) && holds(said, "no account nobody"));
        CHECK(!s.hub->requestSignIn("../a1", &said) && !holds(said, "../a1"));
        // The window claims: the process starts, once; a second window's claim starts nothing.
        CHECK(s.act(sync::ActionKind::SignInClaim, 1, "w1"));
        CHECK(s.act(sync::ActionKind::SignInClaim, 1, "w2"));
        CHECK((s.runner.started == std::vector<std::pair<std::uint64_t, std::string>>{{1, "a1"}}));
        CHECK(s.store.applied("c1") == s.num);
        // The URL: in the claiming window's state alone; never in state(), the other window's, or a state file.
        s.hub->signInUrl("a1", 1, secret(kUrl));
        CHECK(s.hub->state().offers()[0].url.empty() && s.hub->state().offers()[0].opener == sync::kSignInHeldBy);
        // No window learns another's claim token: the other window sees the flow held, by nobody it can name.
        CHECK(s.hub->stateFor("w1").offers()[0].opener == "w1" && s.hub->stateFor("w2").offers()[0].opener == sync::kSignInHeldBy);
        CHECK(s.hub->stateFor("w1").offers()[0].url.bytes() == kUrl);
        CHECK(s.hub->stateFor("w2").offers()[0].url.empty() && s.hub->stateFor("").offers()[0].url.empty());
        CHECK(!leaks(sync::stateFile(s.hub->stateFor("w1"))));
        // Opened, a stray callback dropped, the matching one replayed once to the URL's redirect.
        CHECK(s.act(sync::ActionKind::SignInOpened, 1, "w1"));
        CHECK(s.act(sync::ActionKind::SignInCallback, 1, "w1", "code=" + kCode + "&state=" + kOtherState));
        CHECK(s.act(sync::ActionKind::SignInCallback, 1, "w2", kCallback));
        CHECK(s.runner.replayed.empty());
        CHECK(s.act(sync::ActionKind::SignInCallback, 1, "w1", kCallback));
        CHECK(s.act(sync::ActionKind::SignInCallback, 1, "w1", kCallback));
        CHECK(s.runner.replayed == std::vector<std::string>{"a1 54545/callback " + kCallback});
        // An API key printed: refused, the process ended, a fresh flow (id 2) for the next show.
        s.hub->signInPrinted("a1", secret(kApiKey));
        CHECK(k.files.empty() && s.hub->signedIn().count("a1") == 0);
        CHECK(s.hub->signIns().stateOf(1) == sync::SignInState::Failed && !s.runner.stopped.empty());
        CHECK(s.hub->state().offers().size() == 1 && s.hub->state().offers()[0].id == 2);
        // The second flow, through to the token: stored, signed in, its drivers told, the offers empty.
        CHECK(s.act(sync::ActionKind::SignInClaim, 2, "w2"));
        s.hub->signInUrl("a1", 2, secret(kUrl));
        s.hub->signInPrinted("a1", secret(kToken));
        CHECK(k.files["a1/oauth-token"] == kToken && s.hub->signedIn().count("a1") == 1);
        CHECK(s.responder.logins == std::vector<std::string>{"a1"} && s.hub->state().offers().empty());
        CHECK(s.hub->signIns().stateOf(2) == sync::SignInState::Captured && s.events.replies.empty());
        CHECK(s.hub->requestSignIn("a1", &said) && said == "signed in");
        CHECK(holds(s.transcript(1), "from the browser's sign-in was stored"));
        // A login printed after its flow ended, with no flow live: stored all the same (the person signed in).
        k.files.clear();
        s.hub->signInPrinted("a1", secret(kToken));
        CHECK(k.files["a1/oauth-token"] == kToken && s.hub->signedIn().count("a1") == 1 &&
              s.hub->state().offers().empty());
        // The login lost: a fresh flow, id 3; the window that claimed it goes; another takes it over, nothing starts.
        s.hub->reportSignedIn("a1", false);
        CHECK(s.hub->state().offers().size() == 1 && s.hub->state().offers()[0].id == 3);
        const std::size_t starts = s.runner.started.size();
        CHECK(s.act(sync::ActionKind::SignInClaim, 3, "w1"));
        s.hub->signInWindowGone("w1");
        CHECK(s.hub->state().offers()[0].opener.empty());
        CHECK(s.act(sync::ActionKind::SignInClaim, 3, "w2"));
        CHECK(s.runner.started.size() == starts + 1 && s.hub->stateFor("w2").offers()[0].opener == "w2");
        // Its deadline: expired, its process ended, flow 4 offered.
        s.now += sync::kSignInDeadlineMs;
        s.hub->expireSignIns();
        CHECK(s.hub->signIns().stateOf(3) == sync::SignInState::Expired && s.hub->state().offers()[0].id == 4);
        // One chance: the disk fails as the token arrives; the login is held, the account needs nothing more, and is
        // signed in once a retry writes it.
        CHECK(s.act(sync::ActionKind::SignInClaim, 4, "w2"));
        k.files.clear();
        k.fail = true;
        s.hub->signInPrinted("a1", secret(kToken));
        CHECK(k.files.empty() && s.hub->secrets().holding().size() == 1 && s.hub->state().offers().empty());
        CHECK(s.hub->state().requests().empty() && s.hub->signedIn().count("a1") == 0);
        CHECK(holds(s.transcript(1), "nothing needs signing in again"));
        s.hub->reportSignedIn("a1", false);
        CHECK(s.hub->state().offers().empty() && s.store.applied(sync::kSignInIdsClient) == 4);
        CHECK(s.hub->requestSignIn("a1", &said) && holds(said, "being stored"));
        k.fail = false;
        s.now += sync::kSecretRetryFirstMs;
        s.hub->retrySecrets();
        CHECK(k.files["a1/oauth-token"] == kToken && s.hub->signedIn().count("a1") == 1);
        // A flow live at the end: dropped, its process ended.
        s.hub->reportSignedIn("a1", false);
        CHECK(s.act(sync::ActionKind::SignInClaim, 5, "w2"));
        s.runner.stopped.clear();
        s.hub->dropSignIns();
        CHECK(s.runner.stopped == std::vector<std::string>{"a1"} && s.hub->state().offers().empty());
        // Nothing the hub said gives anything away.
        for (const std::string& l : s.events.lines) {
            CHECK(!leaks(l));
        }
        CHECK(!leaks(s.transcript(1)));
    }
    {
        // A restart over the same store: ids go on from the store's counter, never reused.
        Server s(fs, k);
        CHECK(s.hub->state().offers().size() == 1 && s.hub->state().offers()[0].id == 6);
    }
    {
        // A hub with no sign-in wired: the actions are counted and dropped, and a claude account is asked for by paste.
        store::MemFs other;
        server::DurableStore store;
        std::string why;
        CHECK(store.open(other, "store", &why));
        sync::Hub h(store);
        accounts::Registry r;
        r.accounts = {accounts::Account{"a1", "claude"}};
        CHECK(h.useAccounts(r, nullptr).empty());
        Keeper k2;
        Events ev;
        h.secretsWith(&k2, &ev, [] { return true; }, [] { return std::uint64_t{1000}; });
        sync::Action a;
        a.num = 1;
        a.kind = sync::ActionKind::SignInClaim;
        a.request = 1;
        a.text = "w1";
        CHECK(!h.apply(a, &why, "c1") && holds(why, "dropped") && store.applied("c1") == 1);
        h.reportSignedIn("a1", false);
        CHECK(h.state().requests().size() == 1 && h.state().offers().empty());
    }
}

// A login printed late: the deadline passes while the browser is sent back (the callback forwarded, the process told
// to end), and the process prints its login in the grace before it ends. Stored, the account signed in, the flow made
// for the next show dropped: never a second sign-in (models/SignIn.tla Print's late branch, EveryPrintTaken). An API
// key printed late is dropped, and changes nothing.
void hub_late() {
    store::MemFs fs;
    Keeper k;
    Server s(fs, k);
    CHECK(s.act(sync::ActionKind::SignInClaim, 1, "w1"));
    s.hub->signInUrl("a1", 1, secret(kUrl));
    CHECK(s.act(sync::ActionKind::SignInOpened, 1, "w1"));
    CHECK(s.act(sync::ActionKind::SignInCallback, 1, "w1", kCallback));
    CHECK(s.hub->signIns().stateOf(1) == sync::SignInState::CallbackForwarded);
    s.now += sync::kSignInDeadlineMs;
    s.hub->expireSignIns();
    CHECK(s.hub->signIns().stateOf(1) == sync::SignInState::Expired &&
          s.runner.stopped == std::vector<std::string>{"a1"});
    CHECK(s.hub->state().offers().size() == 1 && s.hub->state().offers()[0].id == 2);
    // An API key, late: dropped; the fresh flow stays.
    s.hub->signInPrinted("a1", secret(kApiKey));
    CHECK(k.files.empty() && s.hub->state().offers().size() == 1 && s.hub->signedIn().count("a1") == 0);
    // The login, late: stored, signed in, flow 2 dropped, nothing offered, nothing asked.
    s.hub->signInPrinted("a1", secret(kToken));
    CHECK(k.files["a1/oauth-token"] == kToken && s.hub->signedIn().count("a1") == 1);
    CHECK(s.hub->signIns().stateOf(2) == sync::SignInState::Dropped && s.hub->state().offers().empty());
    CHECK(s.hub->state().requests().empty() && s.store.applied(sync::kSignInIdsClient) == 2);
    std::string said;
    CHECK(s.hub->requestSignIn("a1", &said) && said == "signed in");
    CHECK(s.responder.logins == std::vector<std::string>{"a1"});
    // Late with the disk failing: held, nothing offered, and written by the retry.
    s.hub->reportSignedIn("a1", false);
    CHECK(s.act(sync::ActionKind::SignInClaim, 3, "w1"));
    s.now += sync::kSignInDeadlineMs;
    s.hub->expireSignIns();
    CHECK(s.hub->signIns().stateOf(3) == sync::SignInState::Expired && s.hub->state().offers().size() == 1);
    k.files.clear();
    k.fail = true;
    s.hub->signInPrinted("a1", secret(kToken));
    CHECK(k.files.empty() && s.hub->secrets().holding().size() == 1 && s.hub->state().offers().empty());
    k.fail = false;
    s.now += sync::kSecretRetryFirstMs;
    s.hub->retrySecrets();
    CHECK(k.files["a1/oauth-token"] == kToken && s.hub->signedIn().count("a1") == 1);
    for (const std::string& l : s.events.lines) {
        CHECK(!leaks(l));
    }
    CHECK(!leaks(s.transcript(1)));
}

// An older window (format 3, from before sign-ins) and a newer server: with only such windows linked, an account is
// asked for by a paste, as before sign-ins; the newer window with an older server is the codecs group's.
void hub_older() {
    // Which windows act on a sign-in is the link's format, the older of the two Hellos' protos, and nothing else
    // (sync::takesSignIn): a window of format 4 (from before sign-ins) with this server does not, one of 5 does.
    const std::uint32_t older = sync::linkFormat(sync::kTablesFormat, sync::kStateFormat);
    const std::uint32_t current = sync::linkFormat(sync::kStateFormat, sync::kStateFormat);
    CHECK(!sync::takesSignIn(older) && sync::takesSignIn(current));
    CHECK(!sync::takesSignIn(sync::linkFormat(sync::kStateFormat, sync::kTablesFormat)));   // an older server's link
    // Only older windows linked from the start: a1 is asked for by a paste, no flow is minted.
    {
        store::MemFs fs;
        Keeper k;
        std::uint32_t linked = older;
        Server s(fs, k, [&linked] { return sync::takesSignIn(linked); });
        CHECK(s.hub->state().offers().empty() && s.store.applied(sync::kSignInIdsClient) == 0);
        const std::vector<sync::SecretRequest> r = s.hub->state().requests();
        CHECK(r.size() == 1 && r[0].account == "a1" && r[0].what == "oauth-token" && holds(r[0].why, "too old"));
        std::string said;
        CHECK(s.hub->requestSignIn("a1", &said) && holds(said, "asked for by a paste"));
        CHECK(s.hub->state().requests().size() == 1 && s.store.applied(sync::kSignInIdsClient) == 0);
        // A newer window links: the paste stays asked for (it shows the dialog too); nothing more is minted.
        linked = current;
        s.hub->signInWindowsChanged();
        CHECK(s.hub->state().requests().size() == 1 && s.hub->state().offers().empty());
        // The person pastes: stored, as any paste is.
        CHECK(s.act(sync::ActionKind::SecretAnswer, r[0].id, "", kToken));
        CHECK(k.files["a1/oauth-token"] == kToken && s.hub->state().requests().empty());
    }
    // A newer window, then only older ones: the needed flow ends and a paste is asked for; a flow a window holds goes
    // on; a lost login while only older ones are linked is asked for by a paste.
    {
        store::MemFs fs;
        Keeper k;
        std::uint32_t linked = current;
        Server s(fs, k, [&linked] { return sync::takesSignIn(linked); });
        CHECK(s.hub->state().offers().size() == 1 && s.hub->state().requests().empty());
        s.hub->reportSignedIn("a2", false);
        CHECK(s.hub->state().offers().size() == 2);
        CHECK(s.act(sync::ActionKind::SignInClaim, 2, "w1"));
        CHECK(s.hub->signIns().stateOf(2) == sync::SignInState::Started);
        s.hub->signInWindowsChanged();      // a newer window still there: nothing changes
        CHECK(s.hub->signIns().stateOf(1) == sync::SignInState::Needed);
        linked = older;
        s.hub->signInWindowsChanged();
        CHECK(s.hub->signIns().stateOf(1) == sync::SignInState::Dropped);
        CHECK(s.hub->signIns().stateOf(2) == sync::SignInState::Started);
        const std::vector<sync::SecretRequest> r = s.hub->state().requests();
        CHECK(r.size() == 1 && r[0].account == "a1");
        CHECK(s.hub->state().offers().size() == 1 && s.hub->state().offers()[0].id == 2);
        bool logged = false;
        for (const std::string& l : s.events.lines) {
            logged = logged || holds(l, "sign-in 1: a1: no window here signs in in the browser");
            CHECK(!leaks(l));
        }
        CHECK(logged);
        CHECK(s.store.applied(sync::kSignInIdsClient) == 2);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The leak test: one whole sign-in with stray callbacks, an API key, a held login and a restart, and every output
// searched: the log lines, every session's transcript, the store's files, every state sent (the other window's whole,
// the claiming window's but for its own URL), the actions' wire in format 2.

void leak() {
    store::MemFs fs;
    Keeper k;
    std::vector<std::string> outputs;
    {
        Server s(fs, k);
        s.act(sync::ActionKind::SignInClaim, 1, "w1");
        s.hub->signInUrl("a1", 1, secret(kUrl));
        outputs.push_back(s.hub->stateFor("w2").init_diff());
        outputs.push_back(sync::stateFile(s.hub->stateFor("w1")));
        s.act(sync::ActionKind::SignInOpened, 1, "w1");
        s.act(sync::ActionKind::SignInCallback, 1, "w2", kCallback);
        s.act(sync::ActionKind::SignInCallback, 1, "w1", "code=" + kCode + "&state=" + kOtherState);
        s.act(sync::ActionKind::SignInCallback, 1, "w1", kCallback);
        s.hub->signInPrinted("a1", secret(kApiKey));
        s.act(sync::ActionKind::SignInClaim, 2, "w1");
        s.hub->signInUrl("a1", 2, secret(kUrl));
        k.fail = true;
        s.hub->signInPrinted("a1", secret(kToken));
        outputs.push_back(s.hub->stateFor("w1").init_diff());
        outputs.push_back(s.hub->state().init_diff());
        outputs.insert(outputs.end(), s.events.lines.begin(), s.events.lines.end());
        outputs.push_back(s.transcript(1));
    }
    {
        Server s(fs, k);
        outputs.push_back(s.hub->state().init_diff());
        outputs.insert(outputs.end(), s.events.lines.begin(), s.events.lines.end());
        outputs.push_back(s.transcript(1));
    }
    std::vector<std::string> files;
    fs.list("store", files);
    CHECK(!files.empty());
    for (const std::string& f : files) {
        store::Bytes bytes;
        if (fs.read_all("store/" + f, bytes).is_ok()) {
            outputs.emplace_back(bytes.begin(), bytes.end());
        }
    }
    for (const std::string& text : outputs) {
        CHECK(!leaks(text));
    }
    CHECK(outputs.size() > 10);
}

void leak_scan() {
    // The scan finds each made-up value inside other text, so that the leak test's silence means something.
    CHECK(leaks("x " + kToken + " y") && leaks("state " + kState.substr(4, 8)) && leaks(kCode));
    CHECK(leaks(test::LeakScan::base64(kToken, false)) && leaks(test::LeakScan::hex(kCode.substr(2, 9), true)));
    CHECK(!leaks("sign-in 1: a1: forwarded") && !leaks("https://claude.com/cai/oauth/authorize"));
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"urls", urls}, {"desk", desk}, {"opener", opener}, {"gate", gate}, {"scanner", scanner},
    {"codecs", codecs}, {"hub", hub}, {"hub_late", hub_late}, {"hub_older", hub_older}, {"leak", leak}, {"leak_scan", leak_scan},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : kGroups) {
        if (argc > 1 && std::strcmp(argv[1], g.name) != 0) {
            continue;
        }
        g.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "signin_test: no group %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "signin_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("signin_test: ok\n");
    return 0;
}
