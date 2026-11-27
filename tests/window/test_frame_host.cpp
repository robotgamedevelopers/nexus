// Unit tests of the window's frame code around the AI tabs (src/frame_host.h FrameHost::takeAIs), over a fake
// link: no window, no Prax tree, no clock. A tab the AIs select (the saved one arriving, or another after the one
// that showed went) is no choice of the person's: the view's next round (view.h selected) must not see a change of the
// selected tab, so that it neither saves the selection nor ends the saved tab's wait. A selection the person made is
// read by the round as before. And text from outside as a log line holds it (loggable).

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "style.h"
#include "test_link_port.h"

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

using test::FakeLink;

struct Window {
    Model model{{}, true};
    View view;
    FakeLink link;
    FrameHost host;
    int chosen = 0;     // the times the round would have taken a selection for the person's

    explicit Window(const std::string& savedAI) {
        view.model = &model;
        host.model = &model;
        host.view = &view;
        host.strip = sync::AIStrip({}, savedAI, {});
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
    w.link.ais({"ada", "bo"});
    w.host.pollLink();
    w.round();
    CHECK(w.chosen == 0);
    // The person chooses ada, then bo.
    CHECK(w.model.select_tab(w.tabOf("ada")));
    w.round();
    CHECK(w.model.select_tab(w.tabOf("bo")));
    w.round();
    CHECK(w.chosen == 2);
    // bo goes from the server's AIs: ada shows again, by the strip, not by the person.
    w.link.ais({"ada"});
    w.host.pollLink();
    CHECK(w.model.tab(w.model.active()).name == "ada");
    w.round();
    CHECK(w.chosen == 2);
}

// The saved tab arrives and the strip selects it: not the person's choice either; a later choice is.
void savedTabArrivingIsNoChoice() {
    Window w("bo");
    CHECK(w.host.strip.pending());
    w.link.ais({"ada", "bo"});
    w.host.pollLink();
    CHECK(w.model.tab(w.model.active()).name == "bo" && !w.host.strip.pending());
    w.round();
    CHECK(w.chosen == 0);
    CHECK(w.model.select_tab(w.tabOf("ada")));
    w.round();
    CHECK(w.chosen == 1);
}

// While the saved tab waits, AIs that bring no tab of its AI's and select nothing leave it waiting.
void waitOutlastsOtherAIs() {
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
    session->info.aiName = "tools";
    w.link.ais({"tools"});
    w.link.state.sessions = sync::Sessions({session});
    w.link.state.sessions.setAIs({sync::ServerAI{"tools"}});
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
    convo->info.aiName = "Ada";
    convo->info.account = "work";
    w.link.state.phase = sync::LinkPhase::Connected;
    w.link.state.sessions = sync::Sessions({convo});
    w.link.state.sessions.setAIs({{"Ada", {{"work", "Work"}, {"home", ""}}}, {"Bo"}});
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
    w.link.state.sessions.setAIs({{"Ada", {{"work", "Work"}, {"home", ""}}}, {"Bo"}});
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
    CHECK((w.view.signInAccount() == SignInAccount{"home", "", "", "", "Ada"}));
    CHECK(w.view.activateSignIn());
    CHECK(w.view.activateSignIn());
    CHECK(w.link.sent.size() == 3 && w.link.sent.back().kind == sync::ActionKind::SignInClaim &&
          w.link.sent.back().request == 11 && w.link.sent.back().session == 19 && w.link.sent.back().account == "home");
    // Explicit selection can reopen a browser page that was closed or rejected.
    wanted.opener = w.host.signIn.opener.token();
    wanted.host = "studio";
    wanted.url = sync::SecretValue("https://claude.com/cai/oauth/authorize?client_id=test"
        "&redirect_uri=http%3A%2F%2Flocalhost%3A54545%2Fcallback&state=fakeStHostXq7wP2");
    int pages = 0;
    w.host.signIn.openUrl = [&](const std::string& url) { CHECK(url == wanted.url.bytes()); ++pages; };
    w.host.signIn.local = "studio";
    w.link.state.sessions.setOffers({other, wanted});
    w.link.changed = true;
    w.host.pollLink();
    w.host.signIn.step();
    CHECK(pages == 1);
    const auto actions = w.link.sent.size();
    w.view.accountAction("sign_in:home");
    CHECK(pages == 2 && w.link.sent.size() == actions);
    w.host.signIn.step();
    CHECK(pages == 2);
    // An older server advertises AIs without choices; no relative command is sent.
    w.link.state.sessions.setAIs({{"Ada"}, {"Bo"}});
    w.link.changed = true;
    w.host.pollLink();
    w.view.accountCommand("next");
    CHECK(w.link.sent.size() == actions && w.view.accountCaption().empty());
}

void accountPickerRechecksTheServerRoster() {
    Window w("chat");
    std::vector<sync::AccountChoice> choices{{"first", "Personal"}, {"second", "Work", true}, {"third", ""}};
    const auto publish = [&](sync::SessionId id, const std::string& account) {
        auto session = std::make_shared<sync::SessionView>();
        session->id = id;
        session->info = {"chat", "codex", account, "", ""};
        w.link.state.phase = sync::LinkPhase::Connected;
        w.link.state.sessions = sync::Sessions({session});
        w.link.state.sessions.setAIs({{"chat", choices}});
        w.link.changed = true;
        w.host.pollLink();
    };
    publish(8, "");
    w.link.sent.clear();
    const auto highlighted = [&]() -> std::string {
        if (!w.view.accountPicker) return {};
        const auto& p = *w.view.accountPicker;
        return p.highlighted < p.options.size() ? p.options[p.highlighted].name : std::string();
    };
    w.view.pickerCommand("next");
    CHECK(highlighted() == "first");
    w.view.pickerCommand("cancel");
    w.view.pickerCommand("previous");
    CHECK(highlighted() == "third" && w.link.sent.empty());
    w.view.chooseAccount("unlinked");
    CHECK(w.link.sent.empty());
    w.view.pickerCommand("cancel");

    publish(8, "first");
    w.view.pickerCommand("open");
    CHECK(highlighted() == "first");
    w.view.pickerCommand("confirm");       // already the confirmed account
    CHECK(w.link.sent.empty() && !w.view.accountPicker);
    w.view.pickerCommand("next");
    CHECK(highlighted() == "second");
    choices.erase(choices.begin() + 1);
    publish(8, "first");
    w.view.chooseAccount("second");        // click from an older rendered roster
    CHECK(w.link.sent.empty());
    w.view.pickerCommand("cancel");

    w.view.pickerCommand("next");
    CHECK(highlighted() == "third");
    publish(9, "first");
    w.view.pickerCommand("confirm");       // cannot confirm the choice for the old session
    CHECK(w.link.sent.empty());
}

void automaticAccountIsASelectionMode() {
    sync::SessionView session;
    session.id = 31;
    session.info.account = "auto";
    session.info.autoAccount = true;
    sync::ServerAI roster{"chat", {{"auto"}, {"work"}}, true};
    auto picker = AccountPicker::open(2, session, roster, 0);
    CHECK(picker && picker->options.size() == 3);
    if (!picker || picker->options.size() != 3) return;
    CHECK(picker->highlighted == 0 && picker->current == sync::kAutoAccountTarget);
    CHECK(picker->rowLabel(picker->options[0], "auto") == "auto");
    CHECK(picker->rowLabel(picker->options[1], "auto") == "auto (account)");
    CHECK(!picker->choice(sync::kAutoAccountTarget));
    CHECK(picker->choice("auto") == "auto");  // pin the actual account named "auto"

    session.info.account = "work";
    CHECK(!picker->refresh(2, session, roster));  // the server rotated while the menu was open
    roster.accounts.resize(1);
    picker = AccountPicker::open(2, session, roster, 0);
    CHECK(picker && picker->options.size() == 1 && !picker->choice(sync::kAutoAccountTarget));
}

void savedStateShowsTheSelectedEmailWithoutStartingSignIn() {
    Model model({}, true);
    View view;
    view.model = &model;
    FrameHost host;
    host.model = &model;
    host.view = &view;
    host.strip = sync::AIStrip({}, "Ada", {});
    auto conversation = std::make_shared<sync::SessionView>();
    conversation->id = 42;
    conversation->info = {"Ada", "codex", "second", "", ""};
    sync::Sessions saved({conversation});
    saved.setAIs({{"Ada", {{"first", "Home", true, "home@example.test"}, {"second", "Work", true, "work@example.test"}}}});
    host.showState(saved, false);
    CHECK((view.signInAccount() == SignInAccount{"second", "Work", "work@example.test", "codex", "Ada"}));
    CHECK(!view.activateSignIn() && !host.signIn.opener.armed());
}

void logoutUsesTheConfirmedAccountAndReturnsToSignIn() {
    Window w("chat");
    auto snapshot = [&](const std::string& harness, const std::string& account, bool needsSignIn, std::uint64_t next, bool automatic = false) {
        auto session = std::make_shared<sync::SessionView>();
        session->id = 31;
        session->info = {"chat", harness, account, "", ""};
        session->info.autoAccount = automatic;
        w.link.state.phase = sync::LinkPhase::Connected;
        w.link.state.sessions = sync::Sessions({session});
        w.link.state.sessions.setAIs({{"chat", {{"home", "Home", false}, {"work", "Work", needsSignIn}}}});
        w.link.state.sessions.setNextAction(next);
        w.link.changed = true;
        w.host.pollLink();
    };
    snapshot("claude", "home", false, 1);
    w.link.sent.clear();
    const auto i = w.model.active();
    w.model.set_draft(i, "/logout");
    w.view.submit(i, true);
    w.view.submit(i, false);
    CHECK(w.link.sent.empty() && w.model.tab(i).draft == "/logout");
    w.view.accountCommand("next");
    CHECK(!w.view.signOut() && w.link.sent.size() == 1);
    snapshot("claude", "work", false, 9);
    CHECK(w.view.signOut() && w.link.sent.size() == 2);
    CHECK(w.link.sent.back().kind == sync::ActionKind::SignOut && w.link.sent.back().session == 31 &&
          w.link.sent.back().account == "work" && w.link.sent.back().text.empty());
    CHECK(!w.view.signOut() && w.link.sent.size() == 2);
    snapshot("claude", "work", true, 10);
    CHECK(w.view.signInAccount().name == "work");
    CHECK(w.view.signOut() && w.link.sent.size() == 3);
    snapshot("muse", "work", false, 11);
    CHECK(w.view.signOut() && w.link.sent.size() == 4);
    snapshot("claude", "work", false, 12, true);
    CHECK(w.view.signOut() && w.link.sent.back().account == "work");
    snapshot("claude", "unlinked", false, 13);
    CHECK(!w.view.signOut() && w.link.sent.size() == 5);
}

void enterDuringAccountSwitchWaitsForServerState() {
    Window w("chat");
    auto session = std::make_shared<sync::SessionView>();
    session->id = 10;
    session->info = {"chat", "grok", "first", "", ""};
    auto snapshot = [&](std::uint64_t next, bool switchAccount) {
        auto current = std::make_shared<sync::SessionView>(*session);
        if (switchAccount) current->info.account = "second";
        w.link.state.phase = sync::LinkPhase::Connected;
        w.link.state.loopback = true;
        w.link.state.sessions = sync::Sessions({current});
        w.link.state.sessions.setAIs({{"chat", {{"first", "", false}, {"second", "", true}}}});
        w.link.state.sessions.setNextAction(next);
        w.link.changed = true;
        w.host.pollLink();
    };
    snapshot(1, false);
    w.link.sent.clear();
    const auto tab = w.model.active();
    w.model.set_draft(tab, "must survive");
    w.view.accountCommand("next");
    snapshot(1, false); // transport activity before the server has applied selection
    w.view.submit(tab, false);
    CHECK(w.link.sent.size() == 1 && w.model.tab(tab).draft == "must survive");
    snapshot(2, true);
    w.view.submit(tab, false);
    CHECK(w.link.sent.size() == 2 && w.link.sent.back().kind == sync::ActionKind::SignInClaim &&
          w.link.sent.back().account == "second" && w.model.tab(tab).draft == "must survive");
    // A fork's account command may leave the current account unchanged.
    snapshot(3, false);
    w.view.accountCommand("stay");
    snapshot(4, false);
    w.view.submit(tab, false);
    CHECK(w.link.sent.size() == 4 && w.link.sent.back().kind == sync::ActionKind::Send);
}

void remoteLoginSwitchAndRetry() {
    Window w("chat");
    auto session = std::make_shared<sync::SessionView>();
    session->id = 37;
    session->info = {"chat", "claude", "first", "", ""};
    w.link.state.phase = sync::LinkPhase::Connected;
    w.link.state.sessions = sync::Sessions({session});
    w.link.state.sessions.setAIs({{"chat", {{"first", "First", true}, {"second", "Second", true}}}});
    sync::SignInOffer first;
    first.id = 1; first.account = "first"; first.provider = "claude"; first.host = "studio";
    first.opener = w.host.signIn.opener.token();
    first.url = sync::SecretValue("https://claude.com/cai/oauth/authorize?client_id=test"
        "&redirect_uri=http%3A%2F%2Flocalhost%3A54545%2Fcallback&state=fakeFirst%26%2B%25");
    auto second = first;
    second.id = 2; second.account = "second";
    second.url = sync::SecretValue("https://claude.com/cai/oauth/authorize?client_id=test"
        "&redirect_uri=http%3A%2F%2Flocalhost%3A54546%2Fcallback&state=fakeSecond");
    w.link.state.sessions.setOffers({first, second});
    w.host.signIn.local = "laptop";
    w.host.signIn.openGate = [](const sync::SignInTarget&) { return std::unique_ptr<SignInGate>(); };
    std::vector<std::string> pages;
    w.host.signIn.openUrl = [&](const std::string& url) { pages.push_back(url); };
    w.link.changed = true;
    w.host.pollBackground();
    CHECK(pages.size() == 1 && w.host.signIn.opener.gate() == first.id);
    w.model.set_draft(w.model.active(), "keep my draft");
    w.link.sent.clear();
    // Closing the browser has no callback. Explicitly choosing another account must release its gate.
    w.view.loginAccount("second");
    w.host.pollBackground();
    CHECK(pages.size() == 2 && pages.back() == second.url.bytes());
    CHECK(w.host.signIn.opener.gate() == second.id);
    CHECK(!w.link.sent.empty() && w.link.sent.front().kind == sync::ActionKind::SignInCallback);
    if (!w.link.sent.empty() && w.link.sent.front().kind == sync::ActionKind::SignInCallback) {
        const auto& cancel = w.link.sent.front();
        CHECK(cancel.request == first.id && cancel.text == first.opener);
        CHECK(sync::queryValue(cancel.secret.bytes(), "error") == "access_denied");
        CHECK(sync::queryValue(cancel.secret.bytes(), "state") == "fakeFirst&+%");
    }
    const auto actions = w.link.sent.size();
    w.view.loginAccount("second");
    CHECK(pages.size() == 3 && pages.back() == second.url.bytes());
    CHECK(w.link.sent.size() == actions); // Retry opens the same page, without another claim or cancellation.
    w.host.pollBackground();
    CHECK(pages.size() == 3 && w.model.tab(w.model.active()).draft == "keep my draft");
    // The server finishes while Safari has focus: service the wake without any UI or frame build.
    auto signedIn = std::make_shared<sync::SessionView>(*session);
    signedIn->info.account = "second";
    w.link.state.sessions = sync::Sessions({signedIn});
    w.link.state.sessions.setAIs({{"chat", {{"first", "First", true}, {"second", "Second", false}}}});
    w.link.changed = true;
    w.host.pollBackground();
    CHECK(w.view.signInAccount().name.empty() && w.view.problem.empty());
    CHECK(!w.host.signIn.opener.gate());
    CHECK(w.model.tab(w.model.active()).draft == "keep my draft");
}

void explicitSignInKeepsTheDraft() {
    Window w("chat");
    auto session = std::make_shared<sync::SessionView>();
    session->id = 37;
    session->info = {"chat", "grok", "work", "", ""};
    w.link.state.phase = sync::LinkPhase::Connected;
    w.link.state.loopback = true;
    w.link.state.sessions = sync::Sessions({session});
    w.link.state.sessions.setAIs({{"chat", {{"work", "Work", true, "work@example.test"}, {"other", "Other", true, "other@example.test"}}}});
    w.link.changed = true;
    w.host.pollLink();
    w.link.sent.clear();
    const auto i = w.model.active();
    CHECK((w.view.signInAccount() == SignInAccount{"work", "Work", "work@example.test", "grok", "chat"}));
    w.view.submit(i, false);
    CHECK(w.model.tab(i).draft.empty());
    w.model.set_draft(i, "  keep this draft  ");
    w.view.submit(i, false);
    CHECK(w.model.tab(i).draft == "  keep this draft  ");
    CHECK(w.link.sent.size() == 1 && w.link.sent.front().kind == sync::ActionKind::SignInClaim &&
          w.link.sent.front().session == 37 && w.link.sent.front().account == "work" && !w.link.sent.front().request);
    w.link.state.sessions.setAIs({{"chat", {{"work", "Work", false}, {"other", "Other", true}}}});
    w.link.changed = true;
    w.host.pollLink();
    CHECK(w.view.signInAccount().name.empty() && !w.view.activateSignIn());
    w.view.submit(i, false);
    CHECK(w.link.sent.size() == 2 && w.link.sent.back().kind == sync::ActionKind::Send &&
          w.link.sent.back().text == "  keep this draft  " && w.model.tab(i).draft.empty());
}

void dismissedAIRemainsLocal() {
    Window w("");
    w.link.ais({"ada"});
    w.host.pollLink();
    const auto i = w.tabOf("ada");
    CHECK(w.model.set_draft(i, "keep my draft"));
    w.view.dismissAI(w.model.tab(i));
    CHECK(w.model.dismiss_tab(i));
    CHECK(!w.host.strip.hasAIs());
    CHECK(w.host.strip.kept().at("ada") == "keep my draft");
    w.link.ais({"bo", "ada"});
    w.host.pollLink();
    CHECK(w.tabOf("ada") == w.model.tab_count());
    CHECK(w.tabOf("bo") < w.model.tab_count());
}


// Locked (View::lockScreen), the host takes and drops every input but the lock screen's own: a typed key, text, a
// paste, a scroll and a drop never reach the board, a terminal or the prompt. The pointer and unmodified Return and
// Escape go on to the lock screen; events that are not input (a wake, a timer) pass. Unlocked, the same key goes on.
void lockedHostDropsInput() {
    Window w("");
    const auto make = [](PrAppEventKind kind, PrAppKey key = 0, PrAppModifierFlags modifiers = 0) {
        PrAppEvent e{};
        e.kind = kind;
        e.keyboard.key = key;
        e.modifiers = modifiers;
        return e;
    };
    w.view.lockScreen = View::LockScreen{"nexus is locked", ""};
    CHECK(w.host.event(make(PrAppEventKind_KeyDown, PrAppKey('a'))));
    CHECK(w.host.event(make(PrAppEventKind_Text)));
    CHECK(w.host.event(make(PrAppEventKind_Paste)));
    CHECK(w.host.event(make(PrAppEventKind_Scroll)));
    CHECK(w.host.event(make(PrAppEventKind_Drop)));
    CHECK(w.host.event(make(PrAppEventKind_CompositionUpdate)));
    CHECK(w.host.event(make(PrAppEventKind_Gesture)));
    CHECK(w.host.event(make(PrAppEventKind_TrackpadTouches)));
    CHECK(w.host.event(make(PrAppEventKind_DragEnter)));
    CHECK(w.host.event(make(PrAppEventKind_DragOver)));
    CHECK(w.host.event(make(PrAppEventKind_DragLeave)));
    CHECK(w.host.event(make(PrAppEventKind_DragPromise)));
    CHECK(w.host.event(make(PrAppEventKind_KeyDown, PrAppKey_Enter, PrAppModifierFlag_Shift)));
    CHECK(w.host.event(make(PrAppEventKind_KeyDown, PrAppKey('t'), style::kCommand)));
    CHECK(!w.host.event(make(PrAppEventKind_KeyDown, PrAppKey_Enter)));
    CHECK(!w.host.event(make(PrAppEventKind_KeyUp, PrAppKey_Escape)));
    CHECK(!w.host.event(make(PrAppEventKind_KeyDown, PrAppKey_Enter, PrAppModifierFlag_CapsLock)));
    CHECK(!w.host.event(make(PrAppEventKind_PointerDown)));
    CHECK(!w.host.event(make(PrAppEventKind_Wake)));
    CHECK(!w.host.event(make(PrAppEventKind_Timer)));
    w.view.lockScreen.reset();
    CHECK(!w.host.event(make(PrAppEventKind_KeyDown, PrAppKey('a'))));
}
// A link that lost actions before the server acknowledged them (LinkState::cut, the native window's link in WSL2 that
// ended): shown whatever the link's phase, never silent. A message among them goes back to its tab as a pending
// message; the problem line says how many were lost and that none was sent again.
void cutActionsAreShown() {
    Window w("");
    w.link.ais({"ada"});
    w.host.pollLink();
    const auto i = w.tabOf("ada");
    const auto id = w.model.tab(i).id;
    w.view.outgoing.push_back({id, 2, "hello across"});
    w.view.outgoing.push_back({id, 4, "still on its way"});
    LinkState s = w.link.state;
    s.phase = sync::LinkPhase::Failed;
    s.problem = "nexus-link-relay in WSL2 ended (exit 1)";
    s.cut = {2, 3};
    w.link.state = s;
    w.link.changed = true;
    w.host.pollLink();
    CHECK(w.model.tab(i).held == "hello across");
    CHECK(w.view.outgoing.size() == 1 && w.view.outgoing.front().action == 4);
    CHECK(w.view.problem.find("acknowledged 2 actions") != std::string::npos);
    CHECK(w.view.problem.find("not sent again") != std::string::npos);
    CHECK(w.view.problem.find("among them are back") != std::string::npos);
    // Each cut comes in one poll: a later state without one leaves the tab and the line as they are.
    s.cut.clear();
    s.phase = sync::LinkPhase::Connected;
    w.link.state = s;
    w.link.changed = true;
    w.host.pollLink();
    CHECK(w.model.tab(i).held == "hello across");
    CHECK(w.view.problem.find("acknowledged 2 actions") != std::string::npos);
    // One cut action, a message: the line says it is back.
    w.view.outgoing.push_back({id, 5, "again"});
    s.cut = {5};
    w.link.state = s;
    w.link.changed = true;
    w.host.pollLink();
    CHECK(w.model.tab(i).held == "again\nhello across");
    CHECK(w.view.problem.find("acknowledged 1 action from") != std::string::npos);
    CHECK(w.view.problem.find("The message is back in its tab") != std::string::npos);
}

} // namespace

int main() {
    dismissedAIRemainsLocal();
    lockedHostDropsInput();
    cutActionsAreShown();
    accountCommandsUseTheActiveConversation();
    automaticAccountIsASelectionMode();
    accountPickerRechecksTheServerRoster();
    explicitSignInKeepsTheDraft();
    remoteLoginSwitchAndRetry();
    enterDuringAccountSwitchWaitsForServerState();
    logoutUsesTheConfirmedAccountAndReturnsToSignIn();
    savedStateShowsTheSelectedEmailWithoutStartingSignIn();
    extensionTargetsItsConfiguredConversation();
    removedTabIsNoChoice();
    savedTabArrivingIsNoChoice();
    waitOutlastsOtherAIs();
    loggableEscapesAndCuts();
    if (failures != 0) {
        std::fprintf(stderr, "frame_host_unit: %d failed\n", failures);
        return 1;
    }
    std::printf("frame_host_unit: ok\n");
    return 0;
}
