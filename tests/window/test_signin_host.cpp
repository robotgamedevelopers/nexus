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
    o.id = 3;
    o.account = provider + "-main";
    o.provider = provider;
    o.opener = opener;
    o.host = host;
    o.url = sync::SecretValue(url);
    o.codeWanted = codeWanted;
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

void privateSignInPages() {
    const std::string nonce(64, 'a');
    const std::string path = "/nexus-signin/" + nonce;
    const std::string url = "http://127.0.0.1:54321" + path;
    for (const char* provider : {"codex", "grok", "muse"}) {
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
    CHECK(sync::parseSignInUrl("codex", url, &target) && target == sync::SignInTarget{});
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

// Antigravity (sync/signin.h signsInByPastedCode): a window anywhere, here one on another machine with no loopback
// link (an iPad), claims, opens Google's own URL with no gate, and once the flow wants the code shows the masked ask
// in the account's tab; Enter sends the code once, with the tab's session, and wipes the field (models/SignInPaste.tla
// Open, Paste).
void pastedCodeAnywhere() {
    const std::string google = "https://accounts.google.com/o/oauth2/auth?client_id=fake-client&state=fakeStHostXq7wP2";
    const std::string code = "4/0AfakeHostCodeQx7wP2mZ";
    Fixture f("studio.local", "ipad");
    std::pair<sync::SessionId, std::string> tab{9, "antigravity-main"};
    f.signIn.activeTab = [&] { return tab; };
    LinkSecrets secrets;
    secrets.port = &f.port;
    secrets.signIn = &f.signIn;
    f.signIn.opener.showAccount("antigravity-main");
    f.signIn.received(offering("", "", "studio", "antigravity"));
    f.signIn.step();
    CHECK(f.port.sent.size() == 1 && f.port.sent.back().kind == sync::ActionKind::SignInClaim);
    f.signIn.received(offering("tokWindowA1", google, "studio", "antigravity"));
    f.signIn.step();
    CHECK(f.opened.size() == 1 && f.opened.front() == google && f.gates.empty());
    CHECK(f.port.sent.size() == 2 && f.port.sent.back().kind == sync::ActionKind::SignInOpened);
    // No code wanted yet: no ask.
    CHECK(secrets.showing() == nullptr && f.signIn.codeAsk() == nullptr);
    f.signIn.received(offering("tokWindowA1", google, "studio", "antigravity", true));
    // Another tab's account: no ask there.
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
    CHECK(f.port.sent.size() == 3 && secrets.showing() == nullptr);
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
    // The desk never replays a callback for it.
    sync::SignInDesk desk;
    desk.need(1, "antigravity-main", "antigravity");
    desk.claim(1, "w1", 1000);
    CHECK(desk.url("antigravity-main", 1, sync::SecretValue(google)).changed);
    CHECK(desk.opened(1, "w1").changed);
    CHECK(desk.callback(1, "w1", sync::SecretValue("code=dummy&state=fakeStHostXq7wP2")).replays.empty());
}

} // namespace

int main() {
    claimsOnceShown();
    opensOnOneMachine();
    gatesAcrossMachines();
    leavesAnotherWindowsFlow();
    lostForgetsTheView();
    refusesALookalike();
    privateSignInPages();
    pastedCodeAnywhere();
    if (failures != 0) {
        std::fprintf(stderr, "signin host: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("signin host: ok\n");
    return 0;
}
