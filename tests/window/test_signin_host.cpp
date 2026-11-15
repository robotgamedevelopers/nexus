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
    void send(sync::Action action) override { sent.push_back(std::move(action)); }
};

sync::Sessions offering(const std::string& opener, const std::string& url, const std::string& host) {
    sync::SignInOffer o;
    o.id = 3;
    o.account = "claude-main";
    o.provider = "claude";
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
    CHECK(f.signIn.problem.find("not one of Claude's") != std::string::npos);
    CHECK(f.signIn.problem.find("evil") == std::string::npos && f.signIn.problem.find("fakeSt") == std::string::npos);
    f.signIn.problem.clear();
    f.signIn.step();
    CHECK(f.signIn.problem.empty() && f.opened.empty());    // once
}

} // namespace

int main() {
    claimsOnceShown();
    opensOnOneMachine();
    gatesAcrossMachines();
    leavesAnotherWindowsFlow();
    lostForgetsTheView();
    refusesALookalike();
    if (failures != 0) {
        std::fprintf(stderr, "signin host: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("signin host: ok\n");
    return 0;
}
