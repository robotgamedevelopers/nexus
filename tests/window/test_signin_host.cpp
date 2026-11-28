// The window's side of in-app sign-in (src/frame_host.h LinkSignIn), unit: a recording port stands in for the link,
// and the browser and the gate are the test's seams, so no URL is opened and no socket bound. The URL is made up.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "frame_host.h"
#include "sync/sessions.h"
#include "sync/signin.h"

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

const std::string kUrl = "https://claude.com/cai/oauth/authorize?code=true&client_id=fake-client"
                         "&redirect_uri=http%3A%2F%2Flocalhost%3A54545%2Fcallback&state=fakeStHostXq7wP2";

struct Recorder : LinkPort {
    std::vector<sync::Action> sent;
    bool poll(LinkState*) override { return false; }
    std::uint64_t send(sync::Action action) override { sent.push_back(std::move(action)); return sent.size(); }
};

sync::Sessions offering(const std::string& opener, const std::string& url, const std::string& host,
                        const std::string& provider = "claude", bool codeWanted = false) {
    sync::SignInOffer o;
    o.codeWanted = codeWanted;
    o.id = 3;
    o.account = provider + "-main";
    o.provider = provider;
    o.opener = opener;
    o.host = host;
    o.url = sync::SecretValue(url);
    sync::Sessions s;
    s.setOffers({o});
    return s;
}

struct Fixture {
    Recorder port;
    LinkSignIn signIn;
    std::vector<std::string> opened;
    std::vector<sync::SignInTarget> gates;
    Fixture(const std::string& serverArg, const std::string& local) {
        signIn.port = &port;
        signIn.serverArg = serverArg;
        signIn.local = local;
        signIn.opener.setToken("tokWindowA1");
        signIn.openUrl = [this](const std::string& url) { opened.push_back(url); };
        signIn.openGate = [this](const sync::SignInTarget& t) {
            gates.push_back(t);
            return std::unique_ptr<SignInGate>();
        };
    }
};

void cancelsOwnedEnrollmentBeforeUrl() {
    for (const auto* provider : {"claude", "codex", "antigravity", "grok", "muse"}) {
        for (const bool supported : {false, true}) for (const bool owned : {false, true}) {
            Fixture f("studio.local", "laptop");
            auto state = offering(owned ? "tokWindowA1" : "another-window", "", "studio", provider);
            auto offers = state.offers();
            offers.front().cancelSupported = supported;
            state.setOffers(offers);
            f.signIn.received(state);
            f.signIn.choose(std::string(provider) + "-main");
            CHECK(f.port.sent.empty());
            f.signIn.choose("replacement");
            CHECK(f.opened.empty());
            if (supported && owned) {
                CHECK(f.port.sent.size() == 1);
                if (!f.port.sent.empty()) {
                    const auto& action = f.port.sent.front();
                    CHECK(action.kind == sync::ActionKind::SignInCancel && action.request == 3);
                    CHECK(action.text == "tokWindowA1" && action.secret.empty());
                }
            } else CHECK(f.port.sent.empty());
        }
    }
}

void claimsOnceShown() {
    Fixture f("studio.local", "studio");
    f.signIn.received(offering("", "", "studio"));
    f.signIn.step();
    CHECK(f.port.sent.empty());     // not shown: it claims nothing
    f.signIn.shown();
    f.signIn.step();
    CHECK(f.port.sent.size() == 1);
    CHECK(f.port.sent[0].kind == sync::ActionKind::SignInClaim);
    CHECK(f.port.sent[0].request == 3);
    CHECK(f.port.sent[0].text == "tokWindowA1");
    f.signIn.step();
    CHECK(f.port.sent.size() == 1); // once
}

void opensOnOneMachine() {
    Fixture f("localhost", "laptop");
    f.signIn.received(offering("tokWindowA1", kUrl, "laptop"));
    f.signIn.step();
    CHECK(f.opened.size() == 1 && f.opened[0] == kUrl);
    CHECK(f.gates.empty());         // Claude Code's own listener takes the redirect
    CHECK(f.port.sent.size() == 1);
    CHECK(f.port.sent[0].kind == sync::ActionKind::SignInOpened);
    CHECK(f.port.sent[0].request == 3);
    CHECK(f.port.sent[0].text.find("54545") == std::string::npos);   // the action carries no URL
    f.signIn.step();
    CHECK(f.opened.size() == 1);    // once
}

void gatesAcrossMachines() {
    Fixture f("studio.local", "laptop");
    f.signIn.received(offering("tokWindowA1", kUrl, "studio"));
    f.signIn.step();
    CHECK(f.gates.size() == 1);
    CHECK(f.gates[0].port == 54545 && f.gates[0].path == "/callback" && f.gates[0].state == "fakeStHostXq7wP2");
    CHECK(f.opened.size() == 1);
    CHECK(f.port.sent.size() == 1 && f.port.sent[0].kind == sync::ActionKind::SignInOpened);
}

void leavesAnotherWindowsFlow() {
    Fixture f("studio.local", "laptop");
    f.signIn.shown();
    f.signIn.received(offering(sync::kSignInHeldBy, "", "studio"));
    f.signIn.step();
    CHECK(f.port.sent.empty());
    CHECK(f.opened.empty());
}

void lostForgetsTheView() {
    Fixture f("studio.local", "laptop");
    f.signIn.received(offering("", "", "studio"));
    f.signIn.lost();
    f.signIn.shown();
    f.signIn.step();
    CHECK(f.port.sent.empty());
}

// A URL to a host not on Claude's list: the window opens nothing, binds no gate, sends nothing up, and says so in a
// plain line that holds none of the URL.
void refusesALookalike() {
    Fixture f("studio.local", "laptop");
    const std::string lookalike = "https://claude.com.evil.example/cai/oauth/authorize?redirect_uri=http%3A%2F%2F"
                                  "localhost%3A54545%2Fcallback&state=fakeStHostXq7wP2";
    f.signIn.received(offering("tokWindowA1", lookalike, "studio"));
    f.signIn.step();
    CHECK(f.opened.empty() && f.gates.empty() && f.port.sent.empty());
    CHECK(f.signIn.problem.find("sign-in address failed validation") != std::string::npos);
    CHECK(f.signIn.problem.find("evil") == std::string::npos && f.signIn.problem.find("fakeSt") == std::string::npos);
    f.signIn.problem.clear();
    f.signIn.step();
    CHECK(f.signIn.problem.empty() && f.opened.empty());    // once
}

void deliberateSwitchOnly() {
    Fixture f("studio", "laptop");
    f.signIn.received(offering("tokWindowA1", kUrl, "studio"));
    f.signIn.step();
    const auto sent = f.port.sent.size();
    f.signIn.choose("claude-main");
    CHECK(f.signIn.opener.gate() == 3 && f.port.sent.size() == sent);
    f.signIn.lock();
    f.signIn.choose("another");
    CHECK(f.signIn.opener.gate() == 3 && f.port.sent.size() == sent);
    f.signIn.unlock();
    f.signIn.choose("another");
    CHECK(!f.signIn.opener.gate() && f.port.sent.size() == sent + 1);
    const auto& cancel = f.port.sent.back();
    CHECK(cancel.kind == sync::ActionKind::SignInCallback && cancel.request == 3);
    sync::SignInDesk desk;
    desk.need(3, "claude-main", "claude");
    desk.claim(3, "tokWindowA1", 0);
    desk.url("claude-main", 3, sync::SecretValue(kUrl));
    desk.opened(3, "tokWindowA1");
    CHECK(desk.callback(3, cancel.text, cancel.secret).replays.size() == 1);
    CHECK(desk.callback(3, cancel.text, cancel.secret).replays.empty());
    desk.exited("claude-main", 3, "declined");
    CHECK(desk.stateOf(3) == sync::SignInState::Cancelled);
    f.signIn.choose("third");
    CHECK(f.port.sent.size() == sent + 1);
}

void pastedCodeAnywhere() {
    const std::string google = "https://accounts.google.com/o/oauth2/auth?client_id=fake-client&state=fakeStHostXq7wP2";
    const std::string code = "4/0AfakeHostCodeQx7wP2mZ";
    Fixture f("studio.local", "ipad");
    std::pair<sync::SessionId, std::string> tab{9, "antigravity-main"};
    f.signIn.forwardPage = [](auto&&...) -> std::unique_ptr<SignInForward> { CHECK(false); return {}; };
    f.signIn.activeTab = [&] { return tab; };
    LinkSecrets secrets;
    secrets.port = &f.port;
    secrets.signIn = &f.signIn;
    f.signIn.opener.showAccount("antigravity-main");
    f.signIn.received(offering("", "", "studio", "antigravity"));
    f.signIn.step();
    CHECK(f.port.sent.size() == 1 && f.port.sent.back().kind == sync::ActionKind::SignInClaim && f.port.sent.back().pastesCode);
    f.signIn.received(offering("tokWindowA1", google, "studio", "antigravity"));
    f.signIn.step();
    CHECK(f.opened.size() == 1 && f.opened.front() == google && f.gates.empty());
    CHECK(f.port.sent.size() == 2 && f.port.sent.back().kind == sync::ActionKind::SignInOpened);
    // No code wanted yet: no ask.
    CHECK(secrets.showing() == nullptr && f.signIn.codeAsk() == nullptr);
    f.signIn.received(offering("tokWindowA1", google, "studio", "antigravity", true));
    // Another tab's account: no ask there.
    f.signIn.claimSessions[tab.second] = tab.first;
    tab = {4, "antigravity-main"};
    CHECK(secrets.showing() == nullptr);
    tab = {4, "claude-main"};
    CHECK(secrets.showing() == nullptr);
    tab = {9, "antigravity-main"};
    const SecretAsk* ask = secrets.showing();
    CHECK(ask != nullptr && (ask->id & LinkSecrets::kCodeAsk) != 0 && ask->what == "sign-in code" &&
          ask->account == "antigravity-main");
    if (ask == nullptr) return;
    // Escape puts it away; Sign in in the tab asks again; what was typed is wiped.
    secrets.type("partial");
    secrets.cancel();
    CHECK(secrets.showing() == nullptr && f.signIn.typedCode().empty() && f.port.sent.size() == 2);
    f.signIn.askCodeAgain(3);
    CHECK(secrets.showing() != nullptr && secrets.typed().empty());
    // Pasted with the line break and spaces a paste brings: sent trimmed, once, and the field wiped.
    secrets.type("  " + code + "\r\n");
    CHECK(secrets.typed() == "  " + code + "\r\n");
    secrets.submit();
    CHECK(f.port.sent.size() == 3);
    const sync::Action& sent = f.port.sent.back();
    CHECK(sent.kind == sync::ActionKind::SignInCode && sent.request == 3 && sent.text == "tokWindowA1" &&
          sent.session == 9 && sent.secret.bytes() == code);
    CHECK(f.signIn.typedCode().empty() && secrets.showing() == nullptr);
    // The flow still says a code is wanted (the server has not answered yet): this window sends no second.
    secrets.submit();
    CHECK(f.port.sent.size() == 3 && secrets.showing() == nullptr && sent.account == "antigravity-main");
    CHECK(f.port.sent.front().pastesCode);      // its claim says it takes a pasted code
    // A state from before the server applied it (action 3 not yet acknowledged) still wanting a code: no ask.
    sync::Sessions pending = offering("tokWindowA1", google, "studio", "antigravity", true);
    pending.setNextAction(3);
    f.signIn.received(pending);
    CHECK(secrets.showing() == nullptr && f.signIn.codeSent(3));
    // The server applied it and the flow still wants a code: it was refused (the refusal's line is in the tab), and
    // the ask opens again, so the person can paste the right one.
    sync::Sessions refused = offering("tokWindowA1", google, "studio", "antigravity", true);
    refused.setNextAction(4);
    f.signIn.received(refused);
    CHECK(!f.signIn.codeSent(3) && secrets.showing() != nullptr);
    // A paste that is no code (two words, a line break inside) is never sent; the window says why, the ask stays.
    secrets.type("Paste this code\ninto your application");
    secrets.submit();
    CHECK(f.port.sent.size() == 3 && f.signIn.typedCode().empty() && secrets.showing() != nullptr);
    CHECK(f.signIn.problem.find("not a sign-in code") != std::string::npos);
    f.signIn.problem.clear();
    secrets.type(code);
    secrets.submit();
    CHECK(f.port.sent.size() == 4 && f.port.sent.back().secret.bytes() == code && f.signIn.codeSent(3));
    // Taken: the state that acknowledges it no longer wants a code, and nothing asks again.
    sync::Sessions taken = offering("tokWindowA1", google, "studio", "antigravity", false);
    taken.setNextAction(5);
    f.signIn.received(taken);
    CHECK(f.signIn.codeSent(3) && secrets.showing() == nullptr);
    // A server that reports nothing applied (nextAction 0): a code sent stays sent.
    Fixture silent("studio.local", "ipad");
    silent.signIn.activeTab = [&] { return tab; };
    silent.signIn.opener.showAccount("antigravity-main");
    silent.signIn.received(offering("tokWindowA1", google, "studio", "antigravity", true));
    silent.signIn.step();
    silent.signIn.typeCode(code);
    silent.signIn.submitCode();
    silent.signIn.received(offering("tokWindowA1", google, "studio", "antigravity", true));
    CHECK(silent.port.sent.size() == 2 && silent.signIn.codeSent(3) && silent.signIn.codeAsk() == nullptr);
    // A flow another window holds never asks here.
    Fixture other("studio.local", "laptop");
    other.signIn.activeTab = [&] { return tab; };
    other.signIn.opener.showAccount("antigravity-main");
    other.signIn.received(offering(sync::kSignInHeldBy, "", "studio", "antigravity", true));
    other.signIn.step();
    CHECK(other.signIn.codeAsk() == nullptr && other.port.sent.empty());
    // Its link lost: the field is wiped.
    Fixture lost("studio.local", "ipad");
    lost.signIn.activeTab = [&] { return tab; };
    lost.signIn.opener.showAccount("antigravity-main");
    lost.signIn.received(offering("tokWindowA1", google, "studio", "antigravity", true));
    lost.signIn.step();
    lost.signIn.typeCode(code);
    CHECK(lost.signIn.typedCode().bytes() == code);
    lost.signIn.lost();
    CHECK(lost.signIn.typedCode().empty() && lost.signIn.codeAsk() == nullptr);
}

void unrelatedSessionCannotCancelEnrollment() {
    Fixture f("studio", "macbook");
    const std::string account = "antigravity-main";
    f.signIn.claimSessions[account] = 10;
    f.signIn.activeTab = [] { return std::pair<sync::SessionId, std::string>{11, "other"}; };
    auto state = offering("tokWindowA1", "", "studio", "antigravity");
    auto offers = state.offers();
    offers.front().cancelSupported = true;
    state.setOffers(offers);
    f.signIn.received(state);
    f.signIn.choose("other");
    CHECK(f.port.sent.empty());
    CHECK(!f.signIn.reopen(offers.front()).empty() && f.opened.empty());
}

void privateSignInPages() {
    const std::string nonce(64, 'a');
    const std::string path = "/nexus-signin/" + nonce;
    const std::string url = "http://127.0.0.1:54321" + path;
    for (const char* provider : {"codex", "antigravity", "grok", "muse"}) {
        CHECK(sync::hasSignIn(provider));
        Fixture f("tester@studio-alias", "studio");
        f.signIn.loopback = true;
        f.signIn.shown();
        f.signIn.received(offering("", "", "studio", provider));
        f.signIn.step();
        CHECK(f.port.sent.empty()); // Showing the overlay never opens an unrelated private provider page.
        f.signIn.opener.showAccount(std::string(provider) + "-main");
        f.signIn.step();
        CHECK(f.port.sent.size() == 1 && f.port.sent.back().kind == sync::ActionKind::SignInClaim);
        f.signIn.received(offering("tokWindowA1", url, "studio", provider));
        f.signIn.step();
        f.signIn.step();
        CHECK(f.opened.size() == 1 && f.opened.front() == url && f.gates.empty());
        CHECK(f.port.sent.size() == 2 && f.port.sent.back().kind == sync::ActionKind::SignInOpened);
        CHECK(f.port.sent.back().text == "tokWindowA1" && f.port.sent.back().secret.empty());

        // Even an already-claimed offer bearing the window's own hostname cannot open a page on another machine.
        Fixture remote("localhost", "studio");   // an SSH alias whose actual endpoint is remote
        remote.signIn.opener.showAccount(std::string(provider) + "-main");
        remote.signIn.received(offering("", "", "studio", provider));
        remote.signIn.step();
        CHECK(remote.signIn.opener.armed());
        remote.signIn.received(offering("tokWindowA1", url, "studio", provider));
        remote.signIn.step();
        CHECK(remote.port.sent.empty() && remote.opened.empty() && remote.gates.empty());

        if (std::string_view(provider) == "codex" || std::string_view(provider) == "antigravity") {
            struct Forward : SignInForward {
                State state = State::Starting;
                State poll() override { return state; }
            };
            Forward* tunnel = nullptr;
            remote.signIn.forwardPage = [&](const std::string& host, std::uint16_t port, bool codexCallback, std::function<void()>) {
                CHECK(codexCallback == (std::string_view(provider) == "codex"));
                CHECK(host == "localhost" && port == 54321);
                auto result = std::make_unique<Forward>();
                tunnel = result.get();
                return result;
            };
            remote.signIn.step();
            CHECK(tunnel && remote.opened.empty() && remote.port.sent.empty());
            if (tunnel) tunnel->state = SignInForward::State::Failed;
            remote.signIn.step();
            CHECK(!remote.signIn.problem.empty() && remote.opened.empty() && remote.port.sent.empty());
            remote.signIn.problem.clear();
            const auto offer = remote.signIn.opener.view().front();
            CHECK(remote.signIn.reopen(offer).empty());
            if (tunnel) tunnel->state = SignInForward::State::Ready;
            remote.signIn.step();
            CHECK(remote.opened.size() == 1 && remote.opened.front() == url);
            CHECK(remote.port.sent.size() == 1 && remote.port.sent.back().kind == sync::ActionKind::SignInOpened);
            remote.signIn.step();
            CHECK(remote.opened.size() == 1);
            CHECK(remote.signIn.reopen(offer).empty() && remote.opened.size() == 2);
            CHECK(remote.port.sent.size() == 1);
            remote.signIn.lost();
        }

        sync::SignInDesk desk;
        desk.need(1, "test-account", provider);
        desk.claim(1, "w1", 1000);
        CHECK(desk.url("test-account", 1, sync::SecretValue(url)).changed);
        CHECK(desk.opened(1, "w1").changed);
        CHECK(desk.callback(1, "w1", sync::SecretValue("code=dummy")).replays.empty());
        CHECK(desk.stateOf(1) == sync::SignInState::Waiting);
    }
    sync::SignInTarget target;
    CHECK(sync::parseSignInUrl("claude", kUrl, &target));
    CHECK(sync::parseSignInUrl("codex", url, &target) && target.port == 54321 && target.state.empty());
    CHECK(!sync::parseSignInUrl("claude", url, &target));
    CHECK(!sync::hasSignIn("unknown") && !sync::parseSignInUrl("unknown", url, &target));
    for (const std::string& bad : {
            kUrl, "http://localhost:54321" + path, "http://127.0.0.1.example:54321" + path,
            "http://127.0.0.1:05432" + path, "http://127.0.0.1:22" + path, "http://127.0.0.1:65536" + path,
            "http://127.0.0.1:54321/" + nonce, url + "#", url + "?code=0", url + "/", url.substr(0, url.size() - 1),
            url.substr(0, url.size() - 1) + "A"}) {
        CHECK(!sync::parseSignInUrl("muse", bad, &target));
    }
}

// The client lock (window_lock.h): locking takes back the claim a show allowed, and while locked no step claims, opens
// or sends anything; unlocking alone allows nothing until the owner's next show.
void lockedClaimsNothing() {
    Fixture f("studio.local", "studio");
    f.signIn.received(offering("", "", "studio"));
    f.signIn.shown();
    f.signIn.lock();
    f.signIn.step();
    CHECK(f.port.sent.empty());
    f.signIn.unlock();
    f.signIn.step();
    CHECK(f.port.sent.empty());     // the show before the lock allows nothing after it
    f.signIn.shown();
    f.signIn.lock();                // shown while locked (the overlay's lock screen): still nothing
    f.signIn.step();
    CHECK(f.port.sent.empty());
    f.signIn.unlock();
    f.signIn.shown();
    f.signIn.step();
    CHECK(f.port.sent.size() == 1 && f.port.sent[0].kind == sync::ActionKind::SignInClaim);
    // An offer this window holds is not opened while locked.
    Fixture g("localhost", "laptop");
    g.signIn.received(offering("tokWindowA1", kUrl, "laptop"));
    g.signIn.lock();
    g.signIn.step();
    CHECK(g.opened.empty() && g.port.sent.empty());
    g.signIn.unlock();
    g.signIn.step();
    CHECK(g.opened.size() == 1);
}

} // namespace

int main() {
    cancelsOwnedEnrollmentBeforeUrl();
    claimsOnceShown();
    lockedClaimsNothing();
    opensOnOneMachine();
    gatesAcrossMachines();
    leavesAnotherWindowsFlow();
    lostForgetsTheView();
    refusesALookalike();
    pastedCodeAnywhere();
    unrelatedSessionCannotCancelEnrollment();
    privateSignInPages();
    deliberateSwitchOnly();
    if (failures != 0) {
        std::fprintf(stderr, "signin host: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("signin host: ok\n");
    return 0;
}
