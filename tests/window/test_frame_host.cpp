// Unit tests of the window's frame code around the friend tabs (src/frame_host.h FrameHost::takeFriends), over a fake
// link: no window, no Prax tree, no clock. A tab the friends select (the saved one arriving, or another after the one
// that showed went) is no choice of the person's: the view's next round (view.h selected) must not see a change of the
// selected tab, so that it neither saves the selection nor ends the saved tab's wait. A selection the person made is
// read by the round as before. And text from outside as a log line holds it (loggable).

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "frame_host.h"

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

struct FakeLink final : LinkPort {
    LinkState state;
    bool changed = false;
    std::vector<sync::Action> sent;

    bool poll(LinkState* out) override {
        if (!changed) {
            return false;
        }
        *out = state;
        changed = false;
        return true;
    }
    void send(sync::Action action) override { sent.push_back(std::move(action)); }

    void friends(const std::vector<std::string>& names) {
        LinkState s;
        s.phase = sync::LinkPhase::Connected;
        std::vector<sync::ServerFriend> list;
        for (const std::string& n : names) {
            list.push_back(sync::ServerFriend{n});
        }
        s.sessions.setFriends(std::move(list));
        state = std::move(s);
        changed = true;
    }
};

struct Window {
    Model model{{}, true};
    View view;
    FakeLink link;
    FrameHost host;
    int chosen = 0;     // the times the round would have taken a selection for the person's

    explicit Window(const std::string& savedFriend) {
        view.model = &model;
        host.model = &model;
        host.view = &view;
        host.strip = sync::FriendStrip({}, savedFriend, {});
        host.connect(&link, "studio");
        view.selectedId = model.tab(model.active()).id;
    }

    // What the view's round does after its strips (view.cpp): a selected tab other than the one at the round before
    // is the person's choice.
    void round() {
        if (model.tab_count() != 0 && model.tab(model.active()).id != view.selectedId) {
            view.selectedId = model.tab(model.active()).id;
            ++chosen;
            host.personSelected();
        }
    }

    std::size_t tabOf(const std::string& name) const {
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            if (model.tab(i).name == name) {
                return i;
            }
        }
        return model.tab_count();
    }
};

// The tab that showed goes: the one the strip shows in its place is not taken for the person's choice.
void removedTabIsNoChoice() {
    Window w("");
    w.link.friends({"ada", "bo"});
    w.host.pollLink();
    w.round();
    CHECK(w.chosen == 0);
    // The person chooses ada, then bo.
    CHECK(w.model.select_tab(w.tabOf("ada")));
    w.round();
    CHECK(w.model.select_tab(w.tabOf("bo")));
    w.round();
    CHECK(w.chosen == 2);
    // bo goes from the server's friends: ada shows again, by the strip, not by the person.
    w.link.friends({"ada"});
    w.host.pollLink();
    CHECK(w.model.tab(w.model.active()).name == "ada");
    w.round();
    CHECK(w.chosen == 2);
}

// The saved tab arrives and the strip selects it: not the person's choice either; a later choice is.
void savedTabArrivingIsNoChoice() {
    Window w("bo");
    CHECK(w.host.strip.pending());
    w.link.friends({"ada", "bo"});
    w.host.pollLink();
    CHECK(w.model.tab(w.model.active()).name == "bo" && !w.host.strip.pending());
    w.round();
    CHECK(w.chosen == 0);
    CHECK(w.model.select_tab(w.tabOf("ada")));
    w.round();
    CHECK(w.chosen == 1);
}

// While the saved tab waits, friends that bring no tab of its friend's and select nothing leave it waiting.
void waitOutlastsOtherFriends() {
    Window w("cy");
    LinkState s;
    s.phase = sync::LinkPhase::Connected;
    s.sessions = sync::Sessions();     // an older server's state: no list, no session
    w.link.state = s;
    w.link.changed = true;
    w.host.pollLink();
    w.round();
    CHECK(w.chosen == 0 && w.host.strip.pending());
}

// Text from outside, as a log line holds it: control bytes, bytes 0x7f and above, and the backslash escaped; at most 80
// bytes, cut between two characters, never inside an escape.
void loggableEscapesAndCuts() {
    CHECK(loggable("ada") == "ada");
    CHECK(loggable("a\nb\x1b[2J") == "a\\x0ab\\x1b[2J");
    CHECK(loggable("caf\xc3\xa9") == "caf\\xc3\\xa9");
    CHECK(loggable("a\\b") == "a\\x5cb");
    CHECK(loggable("\x7f") == "\\x7f");
    CHECK(loggable(std::string(80, 'a')) == std::string(80, 'a'));
    CHECK(loggable(std::string(100, 'a')) == std::string(80, 'a') + "...");
    // An escape that would pass 80 bytes is left out whole.
    CHECK(loggable(std::string(78, 'a') + "\xff" + "b") == std::string(78, 'a') + "...");
    CHECK(loggable(std::string(76, 'a') + "\xff" + "b") == std::string(76, 'a') + "\\xff...");
    CHECK(loggable("").empty());
}

void extensionTargetsItsConfiguredConversation() {
    Window w("");
    CHECK(!w.view.requestService("tools", "one two").empty());
    auto session = std::make_shared<sync::SessionView>();
    session->id = 42;
    session->info.friendName = "tools";
    w.link.friends({"tools"});
    w.link.state.sessions = sync::Sessions({session});
    w.link.state.sessions.setFriends({sync::ServerFriend{"tools"}});
    w.host.pollLink();
    const auto count = w.link.sent.size();
    CHECK(!w.view.requestService("absent", "one two").empty() && w.link.sent.size() == count);
    CHECK(w.view.requestService("tools", "one two").empty() && w.link.sent.size() == count + 1);
    CHECK(w.link.sent.back().kind == sync::ActionKind::Send && w.link.sent.back().session == 42 &&
        w.link.sent.back().text == "one two");
}

void accountCommandsUseTheActiveConversation() {
    Window w("Ada");
    auto convo = std::make_shared<sync::SessionView>();
    convo->id = 19;
    convo->info.friendName = "Ada";
    convo->info.account = "work";
    w.link.state.phase = sync::LinkPhase::Connected;
    w.link.state.sessions = sync::Sessions({convo});
    w.link.state.sessions.setFriends({{"Ada", {{"work", "Work"}, {"home", ""}}}, {"Bo"}});
    w.link.changed = true;
    w.host.pollLink();
    CHECK(w.view.accountCaption() == "Account · Work");
    w.link.sent.clear();
    w.view.accountCommand("next");
    w.view.accountCommand("previous");
    CHECK(w.link.sent.size() == 2 && w.link.sent[0].session == 19 && w.link.sent[0].text == "+next" &&
          w.link.sent[0].kind == sync::ActionKind::SetAccount && w.link.sent[1].text == "+previous");
    // A switch waits for the updated session, without arming any sign-in.
    w.link.changed = true;
    w.host.pollLink();
    CHECK(!w.host.signIn.opener.armed());
    auto switched = std::make_shared<sync::SessionView>(*convo);
    switched->info.account = "home";
    w.link.state.sessions = sync::Sessions({switched});
    w.link.state.sessions.setFriends({{"Ada", {{"work", "Work"}, {"home", ""}}}, {"Bo"}});
    w.link.changed = true;
    w.host.pollLink();
    CHECK(w.view.accountCaption() == "Account · home");
    sync::SignInOffer other;
    other.id = 10; other.account = "work"; other.provider = "claude";
    auto wanted = other;
    wanted.id = 11; wanted.account = "home";
    w.link.state.sessions.setOffers({other, wanted});
    w.link.changed = true;
    w.host.pollLink();
    w.host.signIn.shown();
    w.host.signIn.step();
    CHECK(w.link.sent.size() == 2);
    CHECK(w.view.signInCaption() == "Sign in to home");
    CHECK(w.view.activateSignIn());
    CHECK(w.view.activateSignIn());
    CHECK(w.link.sent.size() == 3 && w.link.sent.back().kind == sync::ActionKind::SignInClaim &&
          w.link.sent.back().request == 11 && w.link.sent.back().session == 19 && w.link.sent.back().account == "home");
    // An older server advertises friends without choices; no relative command is sent.
    w.link.state.sessions.setFriends({{"Ada"}, {"Bo"}});
    w.link.changed = true;
    w.host.pollLink();
    w.view.accountCommand("next");
    CHECK(w.link.sent.size() == 3 && w.view.accountCaption().empty());
}

void explicitSignInKeepsTheDraft() {
    Window w("chat");
    auto session = std::make_shared<sync::SessionView>();
    session->id = 37;
    session->info = {"chat", "grok", "work", "", ""};
    w.link.state.phase = sync::LinkPhase::Connected;
    w.link.state.loopback = true;
    w.link.state.sessions = sync::Sessions({session});
    w.link.state.sessions.setFriends({{"chat", {{"work", "Work", true}, {"other", "Other", true}}}});
    w.link.changed = true;
    w.host.pollLink();
    w.link.sent.clear();
    const auto i = w.model.active();
    CHECK(w.view.signInCaption() == "Sign in to Work");
    w.view.submit(i, false);
    CHECK(w.model.tab(i).draft.empty());
    w.model.set_draft(i, "  keep this draft  ");
    w.view.submit(i, false);
    CHECK(w.model.tab(i).draft == "  keep this draft  ");
    CHECK(w.link.sent.size() == 1 && w.link.sent.front().kind == sync::ActionKind::SignInClaim &&
          w.link.sent.front().session == 37 && w.link.sent.front().account == "work" && !w.link.sent.front().request);
    w.link.state.sessions.setFriends({{"chat", {{"work", "Work", false}, {"other", "Other", true}}}});
    w.link.changed = true;
    w.host.pollLink();
    CHECK(w.view.signInCaption().empty() && !w.view.activateSignIn());
    w.view.submit(i, false);
    CHECK(w.link.sent.size() == 2 && w.link.sent.back().kind == sync::ActionKind::Send &&
          w.link.sent.back().text == "  keep this draft  " && w.model.tab(i).draft.empty());
}

void dismissedFriendRemainsLocal() {
    Window w("");
    w.link.friends({"ada"});
    w.host.pollLink();
    const auto i = w.tabOf("ada");
    CHECK(w.model.set_draft(i, "keep my draft"));
    w.view.dismissFriend(w.model.tab(i));
    CHECK(w.model.dismiss_tab(i));
    CHECK(!w.host.strip.hasFriends());
    CHECK(w.host.strip.kept().at("ada") == "keep my draft");
    w.link.friends({"bo", "ada"});
    w.host.pollLink();
    CHECK(w.tabOf("ada") == w.model.tab_count());
    CHECK(w.tabOf("bo") < w.model.tab_count());
}

} // namespace

int main() {
    dismissedFriendRemainsLocal();
    accountCommandsUseTheActiveConversation();
    explicitSignInKeepsTheDraft();
    extensionTargetsItsConfiguredConversation();
    removedTabIsNoChoice();
    savedTabArrivingIsNoChoice();
    waitOutlastsOtherFriends();
    loggableEscapesAndCuts();
    if (failures != 0) {
        std::fprintf(stderr, "frame_host_unit: %d failed\n", failures);
        return 1;
    }
    std::printf("frame_host_unit: ok\n");
    return 0;
}
