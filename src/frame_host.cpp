#include "frame_host.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "board.h"
#include "terminals.h"

static_assert(nexus::kPromptBytes == nexus::sync::Limits{}.text, "the prompt holds what the server takes");

namespace nexus {

std::string loggable(std::string_view text) {
    static const char* const kHex = "0123456789abcdef";
    constexpr std::size_t kMost = 80;
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool escaped = c < 0x20 || c >= 0x7f || c == '\\';
        if (out.size() + (escaped ? 4 : 1) > kMost) {
            out += "...";   // cut between two characters of the line, never inside an escape
            break;
        }
        if (escaped) {
            out += "\\x";
            out += kHex[c >> 4];
            out += kHex[c & 0xf];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

void FrameHost::keepSelection() {
    if (!keep || selectionStore == nullptr || strip.pending())
        return;
    std::string text = formatSelectedTab(*model);
    if (text == savedSelection)
        return;
    savedSelection = text;
    std::string error;
    if (!saveSelectedTab(*selectionStore, *model, &error))
        std::fprintf(stderr, "nexus: %s\n", error.c_str());
}

void FrameHost::keepTerminals() {
    if (!keep || terminals == nullptr || terminalStore == nullptr)
        return;
    PraxTerminals* t = terminals;
    std::vector<std::string> directories = terminalDirectories(*model,
        [t](std::size_t id) { return t->workingDirectory(id); });
    std::string text = formatTerminals(directories);
    const bool directoriesChanged = text != savedTerminals;
    if (directoriesChanged) {
        savedTerminals = text;
        std::string error;
        if (!saveTerminals(*terminalStore, directories, &error))
            std::fprintf(stderr, "nexus: %s\n", error.c_str());
    }
    const std::size_t focus = model->tab_count() ? model->tab(model->active()).id : ~std::size_t{};
    if (layoutStore && groupPolicyReady && !strip.pending() &&
        (directoriesChanged || savedLayoutRevision != model->layout_revision() || savedLayoutFocus != focus)) {
        layout.capture(*model, directories, rosterKnown);
        std::string error;
        if (layoutStore->write(formatLayout(layout.saved()), &error)) {
            savedLayoutRevision = model->layout_revision();
            savedLayoutFocus = focus;
        } else {
            std::fprintf(stderr, "nexus: could not save the tab layout: %s\n", error.c_str());
        }
    }
    keepSelection();
}

void FrameHost::personSelected() {
    strip.personSelected();
    layout.personSelected();
    if (terminals && terminalStore) keepTerminals();
    else keepSelection();
}

void FrameHost::restoreLayout() {
    if (!layoutStore || !groupPolicyReady) return;
    layout.restore(*model, [this](const std::string& type, std::string* group, std::string* title) {
        const auto it = view->windowApp.types.find(type);
        if (it == view->windowApp.types.end()) return false;
        *group = it->second.group;
        *title = it->second.title;
        return true;
    }, rosterKnown);
    if (model->tab_count()) view->selectedId = model->tab(model->active()).id;
}

void FrameHost::takeFriends(const sync::Sessions& state) {
    std::function<void(sync::Action)> ensure;
    if (link != nullptr) {
        ensure = [this](sync::Action a) { link->send(std::move(a)); };
    }
    sync::StripTake changes = sync::takeFriends(strip, *model, mirror, state, ensure);
    if (!changes.ran) {
        return;
    }
    if (rosterKnown != state.friendsKnown()) savedLayoutRevision = ~std::uint64_t{};
    rosterKnown = state.friendsKnown();
    restoreLayout();
    // A tab the friends selected (the saved one arriving, or another after the one that showed went) is no choice of
    // the person's: the view's next round must not take it for one (view.h selected), so it is neither saved nor ends
    // the saved tab's wait.
    if (view != nullptr && model->tab_count() != 0) {
        view->selectedId = model->tab(model->active()).id;
    }
    for (const std::string& name : changes.dropped) {
        std::fprintf(stderr, "nexus: %s sent the friend \"%s\", which is not a friend's name (1 to 64 letters, digits, "
            "'-', '_' or '.', not starting with '.' or '-'): it has no tab\n", host.c_str(), loggable(name).c_str());
    }
    if (changes.past != 0) {
        std::fprintf(stderr, "nexus: %s sent %zu friends past the %zu a window shows (the most friends.json holds): "
            "they have no tab\n", host.c_str(), changes.past, sync::kMaxFriendTabs);
    }
    if (changes.keptChanged && keep && keptStore != nullptr && keptServer(host)) {
        std::string error;
        if (!saveKeptChange(*keptStore, KeptChange{host, std::move(changes.put), std::move(changes.taken)}, &error)) {
            std::fprintf(stderr, "nexus: %s\n", error.c_str());
        }
    }
    const bool friends = strip.hasFriends();
    if (friends) {
        view->notice.clear();
    } else if (state.friendsKnown()) {
        view->notice = host + " holds no friends: add one to ~/.nexus/server/nexus-config/friends.json there "
            "(docs/accounts.md), then run `nexus-server config reload`.";
    } else {
        view->notice = host + " sends no list of friends (a nexus-server before it), and holds no session to show a "
            "friend by: update nexus-server there.";
    }
}

void LinkSignIn::lost() {
    activeAccount.reset();
    loopback = false;
    opener.lost();
    gate_.reset();
    gateFlow_ = 0;
}

namespace {

bool onThisMachine(const sync::SignInOffer& offer, const void* context) {
    const LinkSignIn* s = static_cast<const LinkSignIn*>(context);
    if (sync::usesLocalSignInPage(offer.provider)) {
        return s->loopback;
    }
    return sync::sameMachine(s->serverArg, offer.host, s->local);
}

} // namespace

void LinkSignIn::step() {
    if (port == nullptr) {
        return;
    }
    // The gate's flow left the state (it ended, or another window took it), or the link was lost: the gate closes.
    if (gate_ != nullptr && opener.gate() != gateFlow_) {
        gate_.reset();
        gateFlow_ = 0;
    }
    if (gate_ != nullptr) {
        sync::Action action;
        std::string why;
        if (gate_->take(&action.secret)) {
            action.kind = sync::ActionKind::SignInCallback;
            action.request = gateFlow_;
            action.text = opener.token();
            port->send(std::move(action));
            gate_.reset();
            gateFlow_ = 0;
            opener.gateClosed();
        } else if (gate_->failed(&why)) {
            // The flow stays where it is and ends at its deadline; the window says why its browser cannot finish.
            problem = why;
            gate_.reset();
            gateFlow_ = 0;
            opener.gateClosed();
        }
    }
    sync::SignInOpener::Step next = opener.next(onThisMachine, this, activeAccount ? &*activeAccount : nullptr);
    if (next.kind == sync::SignInOpener::Step::Kind::Claim) {
        sync::Action action;
        action.kind = sync::ActionKind::SignInClaim;
        action.request = next.id;
        action.text = opener.token();
        port->send(std::move(action));
    } else if (next.kind == sync::SignInOpener::Step::Kind::Refused) {
        // The window's own check of the page's host (sync::parseSignInUrl): nothing is opened, and the flow ends at its
        // deadline. The URL is never shown.
        problem = next.account + "'s sign-in address failed validation. Restart sign-in on the server.";
    } else if (next.kind == sync::SignInOpener::Step::Kind::Open) {
        if (next.gate) {
            gate_ = openGate ? openGate(next.target) : std::make_unique<SignInGate>(next.target);
            gateFlow_ = next.id;
        }
        // Handed to the window's UrlOpener (main.cpp), which opens it off the frame thread: this returns at once.
        if (openUrl) {
            openUrl(next.url.bytes());
        }
        next.url.clear();
        sync::Action action;
        action.kind = sync::ActionKind::SignInOpened;
        action.request = next.id;
        action.text = opener.token();
        port->send(std::move(action));
    }
}

void FrameHost::pollLink() {
    LinkState s;
    if (link == nullptr || !link->poll(&s))
        return;
    // The friends first, so that a friend's new tab shows its session in this same state.
    if (s.phase == sync::LinkPhase::Connected) {
        accountState_ = s.sessions;
        std::erase_if(accountSelections_, [&](const auto& pending) {
            return !s.sessions.nextAction() || !s.sessions.find(pending.first) || s.sessions.nextAction() > pending.second;
        });
        takeFriends(s.sessions);
    }
    mirror.apply(*model, s.sessions);
    if (signIn.port != nullptr) {
        if (s.phase == sync::LinkPhase::Connected) {
            signIn.loopback = s.loopback;
            signIn.received(s.sessions);
            selectSignInAccount();
            for (auto it = pendingClaims_.begin(); it != pendingClaims_.end();) {
                const auto* offer = offerFor(it->first);
                if (offer && !it->second) it->second = offer->id;
                const auto* choice = activeChoice();
                if ((offer && offer->id != it->second) ||
                    (!offer && (it->second || !choice || choice->name != it->first || !choice->needsSignIn)))
                    it = pendingClaims_.erase(it);
                else
                    ++it;
            }
        } else {
            pendingClaims_.clear();
            signIn.lost();
        }
    }
    if (s.phase == sync::LinkPhase::Connected)
        view->promptOpen = true;
    // The open secret requests, from the server's state; a link not connected loses the dialog and what was typed.
    if (secrets.port != nullptr) {
        if (s.phase == sync::LinkPhase::Connected) {
            secrets.prompt.received(s.sessions.requests());
        } else {
            secrets.prompt.lost();
        }
    }
    phase = s.phase;
    refreshPicker();
    heardMsAgo = s.heardMsAgo;
    view->status = sync::linkStatus(s.phase, host, s.heardMsAgo, s.problem);
}

const SecretAsk* LinkSecrets::showing() {
    const sync::SecretRequest* r = prompt.showing();
    if (r == nullptr) {
        return nullptr;
    }
    if (shown_.id != r->id) {
        shown_ = SecretAsk{r->id, r->account, r->what, r->askerName, r->why};
    }
    return &shown_;
}

void LinkSecrets::submit() {
    sync::Action action;
    if (port != nullptr && prompt.submit(&action.request, &action.secret)) {
        action.kind = sync::ActionKind::SecretAnswer;
        port->send(std::move(action));
    }
}

void LinkSecrets::cancel() {
    sync::Action action;
    if (port != nullptr && prompt.cancel(&action.request)) {
        action.kind = sync::ActionKind::SecretCancel;
        port->send(std::move(action));
    }
}

const sync::ServerFriend* FrameHost::activeAccountRoster() const {
    if (!model->tab_count() || model->tab(model->active()).kind != TabKind::Friend)
        return nullptr;
    const auto& name = model->tab(model->active()).name;
    const auto& friends = accountState_.friends();
    const auto it = std::find_if(friends.begin(), friends.end(), [&](const auto& f) { return f.name == name; });
    return it == friends.end() || it->accounts.empty() ? nullptr : &*it;
}

const sync::AccountChoice* FrameHost::activeChoice() const {
    const auto* roster = activeAccountRoster();
    if (!roster) return nullptr;
    const auto* session = accountState_.find(mirror.session(model->tab(model->active()).id));
    if (!session) return nullptr;
    const auto it = std::find_if(roster->accounts.begin(), roster->accounts.end(),
                                 [&](const auto& a) { return a.name == session->info.account; });
    return it == roster->accounts.end() ? nullptr : &*it;
}

const sync::SignInOffer* FrameHost::offerFor(const std::string& account) const {
    const auto& offers = accountState_.offers();
    const auto it = std::find_if(offers.begin(), offers.end(), [&](const auto& o) { return o.account == account; });
    return it == offers.end() ? nullptr : &*it;
}

void FrameHost::selectSignInAccount() {
    // Servers from before the account roster used a show of the overlay to claim a flow.
    const auto& friends = accountState_.friends();
    if (!std::any_of(friends.begin(), friends.end(), [](const auto& f) { return !f.accounts.empty(); })) {
        signIn.activeAccount.reset();
        return;
    }
    const auto* choice = activeChoice();
    signIn.activeAccount = choice ? choice->name : std::string{};
}

bool FrameHost::activateSignIn() {
    if (view->accountPicker) return true;
    if (!link) return false;
    if (model->tab_count() && accountSelections_.contains(mirror.session(model->tab(model->active()).id))) {
        view->problem = "Switching accounts. Your draft is kept. Press Enter when the selected account appears.";
        return true;
    }
    const auto* choice = activeChoice();
    if (!choice) return false;
    const auto* offer = offerFor(choice->name);
    if (!choice->needsSignIn && !offer) return false;
    if (phase != sync::LinkPhase::Connected) {
        view->problem = "Reconnect to the Nexus server to sign in. Your draft is kept.";
        return true;
    }
    const auto session = mirror.session(model->tab(model->active()).id);
    const auto* current = accountState_.find(session);
    if (!current) return false;
    if (sync::usesLocalSignInPage(current->info.harness) && !signIn.loopback) {
        view->problem = "Open Nexus on the server machine to sign in to " + choice->name + ". Your draft is kept.";
        return true;
    }
    if ((offer && !offer->opener.empty()) || pendingClaims_.contains(choice->name)) return true;
    sync::Action action;
    action.kind = sync::ActionKind::SignInClaim;
    action.account = choice->name;
    action.session = session;
    action.request = offer ? offer->id : 0;
    action.text = signIn.opener.token();
    pendingClaims_.emplace(choice->name, action.request);
    link->send(std::move(action));
    return true;
}

void FrameHost::refreshPicker() {
    auto& picker = view->accountPicker;
    if (!picker) return;
    const auto* roster = activeAccountRoster();
    if (phase != sync::LinkPhase::Connected || !roster || (view->secrets && view->secrets->showing())) {
        picker.reset();
        return;
    }
    const auto tab = model->tab(model->active()).id;
    const auto* session = accountState_.find(mirror.session(tab));
    if (!session || !picker->refresh(tab, session->id, roster->accounts, session->info.account))
        picker.reset();
}

void FrameHost::setAccount(sync::SessionId session, const std::string& target) {
    if (!link) return;
    sync::Action action;
    action.kind = sync::ActionKind::SetAccount;
    action.session = session;
    action.text = target;
    const auto number = link->send(std::move(action));
    if (accountState_.nextAction()) accountSelections_[session] = number;
}

void FrameHost::chooseAccount(std::string account) {
    // Revalidate against the latest server snapshot at the time of this click or keypress.
    refreshPicker();
    auto& picker = view->accountPicker;
    if (!picker) return;
    const auto session = picker->session;
    const auto selected = picker->choice(account);
    picker.reset();
    if (selected && !accountSelections_.contains(session))
        setAccount(session, *selected);
}

void FrameHost::pickerCommand(const std::string& command) {
    auto& picker = view->accountPicker;
    if (command == "cancel") {
        picker.reset();
        return;
    }
    const bool wasOpen = picker.has_value();
    refreshPicker();
    if (wasOpen && !picker) return;
    if (command == "confirm") {
        if (picker) chooseAccount(picker->options[picker->highlighted].name);
        return;
    }
    if (command != "open" && command != "previous" && command != "next") return;
    const int direction = command == "next" ? 1 : command == "previous" ? -1 : 0;
    if (picker) {
        picker->step(direction);
        return;
    }
    if (phase != sync::LinkPhase::Connected || (view->secrets && view->secrets->showing())) return;
    const auto* roster = activeAccountRoster();
    if (!roster) return;
    const auto tab = model->tab(model->active()).id;
    const auto* session = accountState_.find(mirror.session(tab));
    if (!session || accountSelections_.contains(session->id)) return;
    picker = AccountPicker::open(tab, session->id, roster->accounts, session->info.account, direction);
}

void FrameHost::wire() {
    view->pickerCommand = [this](const std::string& command) { pickerCommand(command); };
    view->chooseAccount = [this](const std::string& account) { chooseAccount(account); };
    view->dismissFriend = [this](const Tab& tab) {
        strip.dismiss(tab);
        mirror.forget(tab.id);
        if (!tab.draft.empty() && keep && keptStore && keptServer(host)) {
            std::string error;
            if (!saveKeptChange(*keptStore, KeptChange{host, {{tab.name, tab.draft}}, {}}, &error))
                std::fprintf(stderr, "nexus: %s\n", error.c_str());
        }
    };
    view->activateSignIn = [this]() { return activateSignIn(); };
    view->signInAccount = [this]() -> SignInAccount {
        // A captured state shows the same card as the connected session. Activation requires a live link.
        if (phase != sync::LinkPhase::Connected || !model->tab_count() ||
            accountSelections_.contains(mirror.session(model->tab(model->active()).id))) return {};
        const auto* choice = activeChoice();
        if (!choice || (!choice->needsSignIn && !offerFor(choice->name))) return {};
        return {choice->name, choice->label, choice->email};
    };
    view->accountCaption = [this]() -> std::string {
        const auto* roster = activeAccountRoster();
        if (!roster) return {};
        const auto* session = accountState_.find(mirror.session(model->tab(model->active()).id));
        if (!session) return {};
        const auto& name = session->info.account;
        for (const auto& choice : roster->accounts) {
            if (choice.name == name)
                return "Account · " + (choice.label.empty() ? name : choice.label);
        }
        return name.empty() ? "Account · None" : "Account · " + name;
    };
    view->growing = [this](std::size_t id) -> const std::string* {
        const sync::SessionEntry* open = mirror.open(id);
        return open != nullptr ? &open->text : nullptr;
    };
    view->cursor = [this](std::size_t id) { return sync::cursorOf(phase, heardMsAgo, mirror, id); };
}

void FrameHost::connect(LinkPort* port, const std::string& serverHost) {
    link = port;
    host = serverHost;
    secrets.port = port;
    view->secrets = &secrets;
    signIn.port = port;
    signIn.serverArg = serverHost;
    signIn.local = localHostName();
    if (signIn.opener.token().empty()) {
        signIn.opener.setToken(newClaimToken());
    }
    view->promptOpen = false;
    view->status = sync::linkStatus(sync::LinkPhase::Connecting, host, 0, "");
    view->requestService = [this](const std::string& service, const std::string& text) -> std::string {
        if (!link || phase != sync::LinkPhase::Connected)
            return "The server is disconnected. The buffer is kept.";
        if (text.empty() || text.size() > sync::Limits{}.text)
            return "The service takes nonempty text of up to 32 KiB. The buffer is kept.";
        for (std::size_t i = 0; i < model->tab_count(); ++i) {
            if (model->tab(i).kind != TabKind::Friend || model->tab(i).name != service) continue;
            const auto id = sync::sessionOf(*model, i, mirror);
            if (!id) break;
            sync::Action request;
            request.kind = sync::ActionKind::Send;
            request.session = id;
            request.text = text;
            link->send(std::move(request));
            return {};
        }
        return "No conversation with " + service + " is available on the server. The buffer is kept.";
    };
    view->accountCommand = [this](const std::string& command) {
        if (phase != sync::LinkPhase::Connected || !activeAccountRoster())
            return;
        const auto session = mirror.session(model->tab(model->active()).id);
        const auto* current = accountState_.find(session);
        if (!current) return;
        view->accountPicker.reset();
        setAccount(session, "+" + command);
    };
    view->signOut = [this] {
        if (!link || phase != sync::LinkPhase::Connected) {
            view->problem = "Reconnect to the Nexus server before signing out.";
            return false;
        }
        const auto* choice = activeChoice();
        if (!choice) {
            view->problem = "The server has not confirmed a selected account for this conversation.";
            return false;
        }
        const auto session = mirror.session(model->tab(model->active()).id);
        if (accountSelections_.contains(session)) {
            view->problem = "Wait for the server to confirm the account change before signing out.";
            return false;
        }
        if (accountState_.find(session)->info.harness != "claude") {
            view->problem = "Sign-out is currently available for Claude accounts.";
            return false;
        }
        if (!choice->needsSignIn) {
            sync::Action action;
            action.kind = sync::ActionKind::SignOut;
            action.session = session;
            action.account = choice->name;
            const auto number = link->send(std::move(action));
            if (accountState_.nextAction()) accountSelections_[session] = number;
        }
        view->problem.clear();
        return true;
    };
    view->submit = [this](std::size_t i, bool queue) {
        if (i == model->active() && activateSignIn()) return;
        std::string text;
        std::string why;
        // The session the tab shows, the window's own choice; none until the server's state has one of the friend's
        // (the window ensured one as the tab was added), and until then the draft stays.
        const sync::SessionId session = sync::sessionOf(*model, i, mirror);
        if (session == 0 && i < model->tab_count() && model->tab(i).kind == TabKind::Friend) {
            why = "No session with " + model->tab(i).name + " yet: the prompt is kept.";
        } else if (sync::takePrompt(*model, i, sync::Limits{}.text, &text, &why)) {
            view->problem.clear();
            sync::Action action;
            action.kind = queue ? sync::ActionKind::Queue : sync::ActionKind::Send;
            action.session = session;
            action.text = std::move(text);
            link->send(std::move(action));
            return;
        }
        if (!why.empty())
            view->problem = why;
    };
    // Command-Escape: a stop of the session the tab shows, sent whatever the window shows and whether or not the link
    // is up (it waits in order with the other actions); the server decides what it stops. Before any state has held a
    // session of the friend there is no session to name, and nothing is sent.
    view->stop = [this] {
        sync::SessionId session = 0;
        if (sync::stopTarget(*model, model->active(), mirror, &session)) {
            sync::Action action;
            action.kind = sync::ActionKind::Stop;
            action.session = session;
            link->send(std::move(action));
        }
    };
    // The friend tabs, and a session with each, come with the server's state (takeFriends).
    view->notice.clear();
    wire();
}

void FrameHost::showState(const sync::Sessions& state, bool lost) {
    accountState_ = state;
    takeFriends(state);
    mirror.apply(*model, state);
    phase = sync::LinkPhase::Connected;
    refreshPicker();
    heardMsAgo = lost ? sync::kQuietMs : 0;
    wire();
}

void FrameHost::beginFrame(PrKit* kit, PrFontSet* fonts, double time, const PrAppDisplay& display) {
    view->hitch = hitch;
    if (terminals != nullptr) {
        // Each terminal's foreground process, for its tab's label, read where its working directory is read.
        HitchScope scope(hitch, Phase::Terminals);
        terminals->observe(time);
        if (ui != nullptr) {
            terminals->deliver(ui);
        }
    }
    {
        HitchScope scope(hitch, Phase::Keep);
        keepTerminals();
    }
    view->time = time;
    {
        HitchScope scope(hitch, Phase::SyncApply);
        pollLink();
        if (signIn.activeAccount) {
            const auto* choice = activeChoice();
            if (!choice) signIn.activeAccount->clear();
            else if (*signIn.activeAccount != choice->name) *signIn.activeAccount = choice->name;
        }
        signIn.step();
        if (!signIn.problem.empty()) {
            view->problem = std::move(signIn.problem);
            signIn.problem.clear();
        }
    }
    {
        HitchScope scope(hitch, Phase::Layout);
        beginWindowFrame(*view, kit, fonts);
    }
    if (board != nullptr) {
        HitchScope scope(hitch, Phase::Board);
        board->frame(time, display);
    }
}

bool FrameHost::event(const PrAppEvent& e) {
    HitchScope scope(hitch, Phase::Input);
    // While the secret dialog shows, every event is the window's tree's, where the dialog's field has the focus: none
    // reaches the sprint tab's page or a terminal's shell, so a value typed or pasted goes only to the field.
    if (view->secrets != nullptr && view->secrets->showing() != nullptr) {
        return false;
    }
    if (e.kind == PrAppEventKind_KeyDown || e.kind == PrAppEventKind_KeyUp) {
        for (const auto& binding : view->windowApp.shortcuts) {
            if (binding.key == e.keyboard.key && binding.modifiers == (e.modifiers & ~PrAppModifierFlag_CapsLock))
                return false; // Route an application shortcut to the window's capture ahead of the embedded page.
        }
    }
    if (board != nullptr && board->event(e)) {
        return true;
    }
    // Typed into a terminal tab before its screen can take it: held for its shell, in order.
    if (terminals != nullptr && model->tab_count() != 0) {
        const Tab& tab = model->tab(model->active());
        if (tab.kind == TabKind::Terminal && terminals->hold(tab.id, e)) {
            return true;
        }
    }
    if (ui == nullptr || (e.kind != PrAppEventKind_Text && e.kind != PrAppEventKind_Paste) ||
            view->prompt == kPrLayoutNodeNone || prUiFocus(ui) != view->prompt) {
        return false;
    }
    // Text that fits the room left (the selection it replaces counted) goes on as it came.
    const std::size_t room = promptRoom(*view, ui);
    if (e.input.text.size <= room) {
        return false;
    }
    // Text past it is cut to fit and goes on in one event, in its place in the order of events; the transcript says
    // how much was left out.
    const PrStringView kept = cutText(e.input.text, room);
    promptCut(*view, e.input.text.size, kept.size, kept.size != 0);
    if (kept.size != 0) {
        PrAppEvent cut = e;
        cut.input.text = kept;
        prUiPushEvent(ui, kPrUiViewportMain, &cut);
    }
    return true;
}

void FrameHost::build(PrKit* kit, PrLayoutNode root) {
    buildWindow(*view, kit, root);
}

} // namespace nexus
