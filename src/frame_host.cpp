#include "frame_host.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "pr/hash.h"
#include "pr/ui_records.h"

#include "platform.h"
#include "board.h"
#if NEXUS_PROCESSES
#include "command.h"
#include "terminals.h"
#endif

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
#if NEXUS_PROCESSES
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
#endif
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

void FrameHost::takeAIs(const sync::Sessions& state) {
    std::function<void(sync::Action)> ensure;
    if (link != nullptr) {
        ensure = [this](sync::Action a) { link->send(std::move(a)); };
    }
    sync::StripTake changes = sync::takeAIs(strip, *model, mirror, state, ensure);
    if (!changes.ran) {
        return;
    }
    if (rosterKnown != state.aisKnown()) savedLayoutRevision = ~std::uint64_t{};
    rosterKnown = state.aisKnown();
    restoreLayout();
    // A tab the AIs selected (the saved one arriving, or another after the one that showed went) is no choice of
    // the person's: the view's next round must not take it for one (view.h selected), so it is neither saved nor ends
    // the saved tab's wait.
    if (view != nullptr && model->tab_count() != 0) {
        view->selectedId = model->tab(model->active()).id;
    }
    for (const std::string& name : changes.dropped) {
        std::fprintf(stderr, "nexus: %s sent the AI \"%s\", which is not an AI's name (1 to 64 letters, digits, "
            "'-', '_' or '.', not starting with '.' or '-'): it has no tab\n", host.c_str(), loggable(name).c_str());
    }
    if (changes.past != 0) {
        std::fprintf(stderr, "nexus: %s sent %zu AIs past the %zu a window shows (the most ais.json holds): "
            "they have no tab\n", host.c_str(), changes.past, sync::kMaxAITabs);
    }
    if (changes.keptChanged && keep && keptStore != nullptr && keptServer(host)) {
        std::string error;
        if (!saveKeptChange(*keptStore, KeptChange{host, std::move(changes.put), std::move(changes.taken)}, &error)) {
            std::fprintf(stderr, "nexus: %s\n", error.c_str());
        }
    }
    const bool ais = strip.hasAIs();
    if (ais) {
        view->notice.clear();
    } else if (state.aisKnown()) {
        view->notice = host + " has no AIs configured: add one to ~/.nexus/server/nexus-config/ais.json there "
            "(docs/accounts.md), then run `nexus-server config reload`.";
    } else {
        view->notice = host + " sends no list of AIs (a nexus-server before it), and holds no session to show an "
            "AI by: update nexus-server there.";
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

void LinkSignIn::pollGate() {
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
}

void LinkSignIn::choose(const std::string& account) {
    if (!port || locked) return;
    pollGate(); // A callback already received belongs to its account; never replace it with a denial.
    if (gate_ && std::any_of(opener.view().begin(), opener.view().end(), [&](const auto& o) {
            return o.id == gateFlow_ && o.account != account;
        })) {
        sync::Action received;
        if (gate_->stopAndTake(&received.secret)) {
            received.kind = sync::ActionKind::SignInCallback;
            received.request = gateFlow_;
            received.text = opener.token();
            port->send(std::move(received));
            opener.gateClosed();
        }
        gate_.reset();
        gateFlow_ = 0;
    }
    for (const auto& offer : opener.view()) {
        if (offer.cancelSupported && offer.account != account && offer.opener == opener.token()) {
            sync::Action cancel;
            cancel.kind = sync::ActionKind::SignInCancel;
            cancel.request = offer.id;
            cancel.text = opener.token();
            port->send(std::move(cancel));
            if (offer.id == opener.gate()) {
                gate_.reset();
                gateFlow_ = 0;
                opener.gateClosed();
            }
        }
    }
    sync::Action action;
    action.request = opener.cancelFor(account, &action.secret);
    if (action.request) {
        action.kind = sync::ActionKind::SignInCallback;
        action.text = opener.token();
        port->send(std::move(action));
        gate_.reset();
        gateFlow_ = 0;
    }
    activeAccount = account;
}

std::string LinkSignIn::reopen(const sync::SignInOffer& offer) {
    if (locked) return "Unlock Nexus to sign in.";
    if (offer.opener != opener.token()) return "This sign-in is open in another nexus window.";
    if (offer.url.empty() || !opener.opened().contains(offer.id)) return "Opening the sign-in page…";
    sync::SignInTarget target;
    if (!sync::parseSignInUrl(offer.provider, offer.url.bytes(), &target))
        return "The sign-in address failed validation.";
    if (!onThisMachine(offer, this) && opener.gate() != offer.id)
        return "The sign-in callback is no longer waiting. Retry when this attempt ends.";
    if (openUrl) openUrl(offer.url.bytes());
    return {};
}

void LinkSignIn::step() {
    if (port == nullptr || locked) return;
    pollGate();
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
            gate_ = openGate ? openGate(next.target) : std::make_unique<SignInGate>(next.target, wake);
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

void FrameHost::finishUpload() {
    if (!upload_.valid() || upload_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto result = upload_.get();
    auto bubble = std::find_if(view->outgoing.begin(), view->outgoing.end(),
        [&](const auto& b) { return b.tab == result.tab && b.action == 0; });
    if (result.error.empty()) {
        view->attachmentLabels[{result.tab, result.action.text}] = result.original;
        const auto number = link->send(std::move(result.action));
        if (bubble != view->outgoing.end()) bubble->action = number;
        return;
    }
    if (bubble != view->outgoing.end()) view->outgoing.erase(bubble);
    for (size_t i = 0; i < model->tab_count(); ++i)
        if (model->tab(i).id == result.tab) model->restore_held(i, std::move(result.original));
    view->problem = std::move(result.error);
}

void FrameHost::showCut(const std::vector<std::uint64_t>& cut) {
    std::size_t restored = 0;
    for (const std::uint64_t number : cut) {
        auto bubble = std::find_if(view->outgoing.begin(), view->outgoing.end(),
            [&](const auto& b) { return b.action == number; });
        if (bubble == view->outgoing.end()) continue;
        for (std::size_t i = 0; i < model->tab_count(); ++i)
            if (model->tab(i).id == bubble->tab) model->restore_held(i, bubble->text);
        view->outgoing.erase(bubble);
        ++restored;
    }
    const std::size_t n = cut.size();
    view->problem = "The link to the server ended before the server acknowledged " + std::to_string(n) +
        (n == 1 ? " action" : " actions") + " from this window; " + (n == 1 ? "it" : "they") +
        " may not have arrived and " + (n == 1 ? "was" : "were") + " not sent again.";
    if (restored > 0)
        view->problem += restored == n ? (n == 1 ? " The message is back in its tab: check the conversation, then "
            "press Enter to send it." : " The messages are back in their tabs: check each conversation, then press "
            "Enter to send them.") : " The messages among them are back in their tabs: check each conversation, then "
            "press Enter to send them.";
}

void FrameHost::pollLink() {
    finishUpload();
    LinkState s;
    if (link == nullptr || !link->poll(&s))
        return;
    if (!s.cut.empty()) showCut(s.cut);
    // The first dashboard's address, as the server says it (empty: none), for the sprint tab's page, until the strip
    // holds a tab for each dashboard.
    if (board != nullptr && s.phase == sync::LinkPhase::Connected)
        board->offer(s.sessions.dashboards().empty() ? std::string() : s.sessions.dashboards().front().url);
    // The AIs first, so that an AI's new tab shows its session in this same state.
    if (s.phase == sync::LinkPhase::Connected) {
        for (const auto& [name, claim] : pendingClaims_) {
            const auto needed = [&](const sync::Sessions& state) -> std::optional<bool> {
                const auto* primary = state.first(kPrimaryChat);
                if (primary && primary->info.primaryAccount && primary->info.primaryAccount->name == name)
                    return primary->info.primaryAccount->needsSignIn;
                for (const auto& f : state.ais())
                    for (const auto& choice : f.accounts)
                        if (choice.name == name) return choice.needsSignIn;
                return {};
            };
            if (needed(accountState_) == true && needed(s.sessions) == false)
                view->chirp(view->windowApp.surface.readyChirp);
        }
        if (phase == sync::LinkPhase::Connected) {
            for (const auto& session : s.sessions.sessions()) {
                const auto* previous = accountState_.find(session->id);
                if (!previous) continue;
                for (auto index = std::max(previous->total(), session->first); index < session->total(); ++index) {
                    if (session->entries[index - session->first]->kind == EntryKind::Person)
                        view->chirp(view->windowApp.surface.sendChirp);
                }
            }
        }
        std::erase_if(view->outgoing, [&](const auto& b) {
            return b.action && (!s.sessions.nextAction() || b.action < s.sessions.nextAction());
        });
        accountState_ = s.sessions;
        std::erase_if(accountSelections_, [&](const auto& pending) {
            return !s.sessions.nextAction() || !s.sessions.find(pending.first) || s.sessions.nextAction() > pending.second;
        });
        std::erase_if(limitSelections_, [&](const auto& pending) {
            const auto* session = s.sessions.find(pending.first);
            return !session || session->info.limitRequest != pending.second.request ||
                s.sessions.nextAction() > pending.second.action;
        });
        takeAIs(s.sessions);
    }
    mirror.apply(*model, s.sessions);
    if (s.phase == sync::LinkPhase::Connected) {
        for (std::size_t i = 0; i < model->tab_count(); ++i) {
            const auto id = model->tab(i).id;
            if (!mirror.missingHistory(id)) continue;
            const auto* session = s.sessions.find(mirror.session(id));
            if (session && session->first) link->requestHistory(session->id, session->info.contextEpoch, session->first);
        }
    }
    if (signIn.port != nullptr) {
        if (s.phase == sync::LinkPhase::Connected) {
            signIn.loopback = s.loopback;
            signIn.received(s.sessions);
            selectSignInAccount();
            for (auto it = pendingClaims_.begin(); it != pendingClaims_.end();) {
                const auto* offer = offerFor(it->first);
                if (offer && !it->second) it->second = offer->id;
                const auto* choice = activeChoice();
                const bool login = pendingLogin_ && pendingLogin_->second == it->first;
                if ((offer && offer->id != it->second) || (!offer && it->second) ||
                    (!offer && !login && (!choice || choice->name != it->first || !choice->needsSignIn)))
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
    if (signInSelection_) {
        const auto& [id, account] = *signInSelection_;
        if (phase != sync::LinkPhase::Connected || !model->tab_count() ||
                mirror.session(model->tab(model->active()).id) != id) {
            signInSelection_.reset();
        } else if (!accountSelections_.contains(id)) {
            const auto* choice = activeChoice();
            if (choice && choice->name == account && choice->needsSignIn) activateSignIn();
            signInSelection_.reset();
        }
    }
    prepareLogin();
    settleLogin();
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

const sync::ServerAI* FrameHost::activeAccountRoster() const {
    if (!model->tab_count() || model->tab(model->active()).kind != TabKind::AI)
        return nullptr;
    const auto& name = model->tab(model->active()).name;
    const auto& ais = accountState_.ais();
    const auto it = std::find_if(ais.begin(), ais.end(), [&](const auto& f) { return f.name == name; });
    return it == ais.end() || it->accounts.empty() ? nullptr : &*it;
}

const sync::AccountChoice* FrameHost::activeChoice() const {
    if (!model->tab_count()) return nullptr;
    const auto& tab = model->tab(model->active());
    const auto* session = accountState_.find(mirror.session(tab.id));
    if (!session) return nullptr;
    if (tab.primaryChat()) {
        const auto& choice = session->info.primaryAccount;
        return choice && choice->name == session->info.account ? &*choice : nullptr;
    }
    const auto* roster = activeAccountRoster();
    if (!roster) return nullptr;
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
    const auto& ais = accountState_.ais();
    if (!accountState_.first(kPrimaryChat) &&
        !std::any_of(ais.begin(), ais.end(), [](const auto& f) { return !f.accounts.empty(); })) {
        signIn.activeAccount.reset();
        return;
    }
    signIn.activeAccount = signInTarget().value_or(std::string{});
}

// The account whose sign-in page this window opens for the AI that shows: one /login signs in, else the account the
// AI's session is on.
std::optional<std::string> FrameHost::signInTarget() const {
    if (loginPreparation_) return std::string{};
    if (pendingLogin_ && model->tab_count() && mirror.session(model->tab(model->active()).id) == pendingLogin_->first)
        return pendingLogin_->second;
    const auto* choice = activeChoice();
    if (!choice) return std::nullopt;
    return choice->name;
}

bool FrameHost::activateSignIn() {
    if (loginPreparation_) {
        view->problem = "Finish or cancel the account switch before signing in.";
        return true;
    }
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
    if (offer && !offer->opener.empty()) {
        if (offer->opener != signIn.opener.token()) {
            view->problem = "This sign-in is open in another nexus window.";
            return true;
        }
        signIn.choose(choice->name);
        view->problem = signIn.reopen(*offer);
        return true;
    }
    signIn.choose(choice->name);
    if (pendingClaims_.contains(choice->name)) {
        view->problem = "Waiting for the server to open sign-in…";
        return true;
    }
    view->problem.clear();
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
    if (!session || !picker->refresh(tab, *session, *roster, view->windowApp.centeredAccounts))
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
    const bool current = picker->current == account;
    const auto selected = picker->choice(account);
    picker.reset();
    if (view->windowApp.centeredAccounts && view->windowApp.signInOnSelect) {
        if (current || selected) startLogin(account);
        return;
    }
    if (selected && !accountSelections_.contains(session)) {
        setAccount(session, *selected);
        if (view->windowApp.signInOnSelect)
            signInSelection_ = std::make_pair(session, *selected);
    }
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
    picker = previewAccounts(direction);
}

std::optional<AccountPicker> FrameHost::previewAccounts(int direction) const {
    if (loginPreparation_) return std::nullopt;
    if (phase != sync::LinkPhase::Connected || (view->secrets && view->secrets->showing())) return std::nullopt;
    const auto* roster = activeAccountRoster();
    if (!roster) return std::nullopt;
    const auto tab = model->tab(model->active()).id;
    const auto* session = accountState_.find(mirror.session(tab));
    if (!session || accountSelections_.contains(session->id)) return std::nullopt;
    return AccountPicker::open(tab, *session, *roster, direction, view->windowApp.centeredAccounts);
}

const sync::ServerAI* FrameHost::rosterOf(sync::SessionId id) const {
    const auto* session = accountState_.find(id);
    if (!session) return nullptr;
    const auto& ais = accountState_.ais();
    const auto it = std::find_if(ais.begin(), ais.end(),
                                 [&](const auto& f) { return f.name == session->info.aiName; });
    return it == ais.end() || it->accounts.empty() ? nullptr : &*it;
}

// Claim enrollment for the selected account, or request the server's saved-account switch from /account.
std::uint64_t FrameHost::claimAccount(sync::SessionId session, const std::string& account) {
    if (!link) return 0;
    sync::Action action;
    action.kind = sync::ActionKind::SignInClaim;
    action.account = account;
    action.session = session;
    action.request = 0;
    action.text = signIn.opener.token();
    return link->send(std::move(action));
}

// An explicit /login retires the previous saved login before selecting and enrolling the requested account.
void FrameHost::startLogin(const std::string& account) {
    if (!link || phase != sync::LinkPhase::Connected || !model->tab_count()) {
        view->problem = "Reconnect to the Nexus server to sign in.";
        return;
    }
    if (loginPreparation_) {
        view->problem = "Finish or cancel the current account switch first.";
        return;
    }
    const auto session = mirror.session(model->tab(model->active()).id);
    const auto* current = accountState_.find(session);
    const auto* roster = activeAccountRoster();
    if (!current || !roster || accountSelections_.contains(session)) return;
    const auto target = std::find_if(roster->accounts.begin(), roster->accounts.end(),
        [&](const auto& a) { return a.name == account && pickable(a); });
    if (target == roster->accounts.end()) return;
    if (current->info.account == account) {
        activateSignIn(); // Retrying the selected account is not a switch and preserves its accepted callback.
        return;
    }
    if (!accountState_.nextAction()) {
        view->problem = "Update the Nexus server before switching logins; this server cannot confirm account actions.";
        return;
    }
    if (sync::usesLocalSignInPage(current->info.harness) && !signIn.loopback) {
        view->problem = "Open Nexus on the server machine to sign in to " + accountShownAs(*target) + ".";
        return;
    }
    for (const auto& name : {current->info.account, account}) {
        const auto* offer = offerFor(name);
        if (offer && !offer->opener.empty() && offer->opener != signIn.opener.token()) {
            view->problem = "Finish this account's sign-in in the other Nexus window first.";
            return;
        }
        if (offer && !offer->opener.empty() && offer->id != signIn.opener.gate() && !offer->cancelSupported) {
            view->problem = "A sign-in is still starting or finishing. Wait for its result, then retry /login.";
            return;
        }
    }
    pendingLogin_.reset();
    signInSelection_.reset();
    signIn.choose(""); // Preserve an accepted callback; otherwise cancel the previous browser flow.
    loginPreparation_.emplace(LoginPreparationState{session, current->info.account, account,
        LoginPreparation(current->info.harness == "claude")});
    view->problem = "Signing out before switching accounts…";
    prepareLogin();
}

void FrameHost::prepareLogin(bool browserConfirmed) {
    if (!loginPreparation_) return;
    auto& p = *loginPreparation_;
    using Phase = LoginPreparation::Phase;
    const auto* session = accountState_.find(p.session);
    const auto* roster = rosterOf(p.session);
    const sync::AccountChoice *previous = nullptr, *target = nullptr;
    if (roster) for (const auto& a : roster->accounts) {
        if (a.name == p.previous) previous = &a;
        if (a.name == p.target) target = &a;
    }
    const auto before = p.machine.phase();
    bool success = p.signedOut;
    if (before == Phase::SigningOut && session) {
        for (auto n = std::max(p.noticeFrom, session->first); n < session->total(); ++n) {
            const auto& entry = *session->entries[n - session->first];
            if (entry.kind == EntryKind::Notice && entry.text == "Signed out of " + p.previous + " in Nexus.")
                success = true;
        }
    }
    const bool active = model->tab_count() && mirror.session(model->tab(model->active()).id) == p.session;
    bool busy = false;
    for (const auto& name : {p.previous, p.target}) {
        const auto* offer = offerFor(name);
        if (offer && !offer->opener.empty()) busy = true;
    }
    const bool beforeSelection = before == Phase::Waiting || before == Phase::SigningOut;
    const auto after = p.machine.step({
        .valid = phase == sync::LinkPhase::Connected && active && session && target && pickable(*target) &&
            (p.previous.empty() || previous) &&
            (!beforeSelection || session->info.account == p.previous),
        .busy = busy,
        .acknowledged = p.action ? accountState_.nextAction() > p.action : p.previous.empty(),
        .signedOut = p.previous.empty() || (success && previous && previous->needsSignIn),
        .selected = session && session->info.account == p.target,
        .confirmed = browserConfirmed,
    });
    if (after == before) return;
    if (after == Phase::SigningOut) {
        if (p.previous.empty()) { prepareLogin(); return; }
        p.noticeFrom = session->total();
        sync::Action action;
        action.kind = sync::ActionKind::SignOut;
        action.session = p.session;
        action.account = p.previous;
        p.action = link->send(std::move(action));
        accountSelections_[p.session] = p.action;
    } else if (after == Phase::Selecting) {
        p.signedOut = true;
        sync::Action action;
        action.kind = sync::ActionKind::SetAccount;
        action.session = p.session;
        action.text = p.target;
        p.action = link->send(std::move(action));
        accountSelections_[p.session] = p.action;
    } else if (after == Phase::Browser) {
        view->problem.clear();
        view->accounts.dialog = AccountMenuView::Dialog::BrowserLogout;
        view->accounts.dialogTab = model->tab(model->active()).id;
        view->accounts.dialogShown = false;
        if (view->openBrowserLogout) view->openBrowserLogout();
    } else if (after == Phase::Ready) {
        const auto account = p.target;
        loginPreparation_.reset();
        view->accounts.dialog = AccountMenuView::Dialog::None;
        enrollSelected(account);
    } else if (after == Phase::Failed) {
        loginPreparation_.reset();
        view->accounts.dialog = AccountMenuView::Dialog::None;
        view->problem = "The account switch stopped because the server did not confirm it or the selection changed. "
                        "Check the latest notice, then retry /login.";
    }
}

void FrameHost::enrollSelected(const std::string& account) {
    if (!link || phase != sync::LinkPhase::Connected || !model->tab_count()) {
        view->problem = "Reconnect to the Nexus server to sign in.";
        return;
    }
    const auto session = mirror.session(model->tab(model->active()).id);
    const auto* current = accountState_.find(session);
    const auto* roster = activeAccountRoster();
    if (!current || !roster) return;
    const auto it = std::find_if(roster->accounts.begin(), roster->accounts.end(),
                                 [&](const auto& a) { return a.name == account; });
    if (it == roster->accounts.end() || !pickable(*it)) return;
    if (!it->needsSignIn) { view->problem.clear(); return; }
    if (current->info.account != account) return;
    if (sync::usesLocalSignInPage(current->info.harness) && !signIn.loopback) {
        view->problem = "Open Nexus on the server machine to sign in to " + accountShownAs(*it) + ".";
        return;
    }
    const auto* offer = offerFor(account);
    if (offer && !offer->opener.empty() && offer->opener != signIn.opener.token()) {
        view->problem = "This sign-in is open in another nexus window.";
        return;
    }
    pendingLogin_ = std::make_pair(session, account);
    signIn.choose(account);
    view->problem = "Signing in to " + accountShownAs(*it) + "…";
    if (offer && offer->opener == signIn.opener.token()) {
        view->problem = signIn.reopen(*offer);
        return;
    }
    if (pendingClaims_.contains(account)) return;
    pendingClaims_.emplace(account, 0);
    pendingLoginAction_ = claimAccount(session, account);
}

// Enrollment completes only when the server confirms both the selected account and its verified login.
void FrameHost::settleLogin() {
    if (!pendingLogin_) return;
    const auto [id, account] = *pendingLogin_;
    const auto* roster = rosterOf(id);
    const auto* session = accountState_.find(id);
    const sync::AccountChoice* choice = nullptr;
    if (roster) {
        const auto it = std::find_if(roster->accounts.begin(), roster->accounts.end(),
                                     [&](const auto& a) { return a.name == account; });
        if (it != roster->accounts.end()) choice = &*it;
    }
    if (phase != sync::LinkPhase::Connected || !session || !choice) {
        pendingLogin_.reset();
        return;
    }
    if (session->info.account == account && !choice->needsSignIn) {
        pendingLogin_.reset();
        view->problem.clear();
        return;
    }
    const auto* offer = offerFor(account);
    if (choice->needsSignIn && pendingLoginAction_ && accountState_.nextAction() > pendingLoginAction_ &&
            (!offer || offer->opener != signIn.opener.token())) {
        pendingClaims_.erase(account);
        pendingLogin_.reset();
        view->problem = "The sign-in to " + accountShownAs(*choice) + " did not finish. This account remains selected; retry /login.";
    }
}

void FrameHost::wireAccountMenu() {
    view->accountMenuRows = [this]() -> std::optional<std::vector<AccountMenuRow>> {
        if (phase != sync::LinkPhase::Connected || !model->tab_count()) return std::nullopt;
        const auto* roster = activeAccountRoster();
        const auto* session = accountState_.find(mirror.session(model->tab(model->active()).id));
        if (!roster || !session || accountSelections_.contains(session->id)) return std::nullopt;
        return accountMenuRows(*roster, session->info.account);
    };
    view->accountLoginRows = [this]() -> std::vector<AccountMenuRow> {
        const auto* roster = phase == sync::LinkPhase::Connected ? activeAccountRoster() : nullptr;
        return roster ? accountLoginRows(*roster) : std::vector<AccountMenuRow>{};
    };
    view->switchAccount = [this](const std::string& account) {
        if (loginPreparation_) return;
        if (phase != sync::LinkPhase::Connected || !model->tab_count()) return;
        const auto* roster = activeAccountRoster();
        const auto* session = accountState_.find(mirror.session(model->tab(model->active()).id));
        if (!roster || !session || session->info.account == account || accountSelections_.contains(session->id)) return;
        const auto it = std::find_if(roster->accounts.begin(), roster->accounts.end(),
                                     [&](const auto& a) { return a.name == account; });
        if (it == roster->accounts.end() || !pickable(*it) || it->needsSignIn) return;
        view->accountPicker.reset();
        const auto number = claimAccount(session->id, account);
        if (accountState_.nextAction()) accountSelections_[session->id] = number;
    };
    view->loginAccount = [this](const std::string& account) { startLogin(account); };
    view->browserLogoutConfirmed = [this] { prepareLogin(true); };
    view->cancelLoginPreparation = [this] {
        if (!loginPreparation_) return;
        loginPreparation_.reset();
        view->problem = "Sign-in cancelled. The previous saved login is not restored.";
    };
    view->openBrowserLogout = [this] {
        if (signIn.openUrl) signIn.openUrl("https://claude.ai/");
        else view->problem = "Open claude.ai in your browser and sign out before continuing.";
    };
    view->logoutTarget = [this](std::string& account, std::string& shown, std::string& next) {
        if (!link || phase != sync::LinkPhase::Connected) {
            view->problem = "Reconnect to the Nexus server before signing out.";
            return false;
        }
        const auto* choice = activeChoice();
        if (!choice) {
            view->problem = "The server has not confirmed a selected account for this conversation.";
            return false;
        }
        account = choice->name;
        shown = accountShownAs(*choice);
        const auto* roster = activeAccountRoster();
        logoutNext_ = roster ? accountAfterLogout(*roster, choice->name) : std::string{};
        next.clear();
        if (roster) {
            for (const auto& a : roster->accounts)
                if (a.name == logoutNext_) next = accountShownAs(a);
        }
        return true;
    };
    view->logout = [this] {
        if (!model->tab_count() || !view->signOut) return false;
        const auto session = mirror.session(model->tab(model->active()).id);
        const auto next = logoutNext_;
        if (!view->signOut()) return false;
        // The server applies the actions in order: the sign-out leaves the session on its account, asking for its
        // sign-in; the claim of the next signed-in account then switches it there at once.
        if (!next.empty()) {
            const auto number = claimAccount(session, next);
            if (accountState_.nextAction()) accountSelections_[session] = number;
        }
        return true;
    };
}

LimitOffer FrameHost::currentLimit() const {
    if (!model->tab_count()) return {};
    const auto& tab = model->tab(model->active());
    if (tab.kind != TabKind::AI) return {};
    const auto* session = accountState_.find(mirror.session(tab.id));
    if (!session || !session->info.limitRequest) return {};
    const auto& info = session->info;
    return {session->id, info.limitRequest, info.account, info.limitAccount, info.limitModel,
        limitSelections_.contains(session->id) || accountSelections_.contains(session->id),
        phase == sync::LinkPhase::Connected};
}

void FrameHost::wire() {
    view->limitOffer = [this] { return currentLimit(); };
    view->resolveLimit = [this](const LimitOffer& shown, const std::string& choice) {
        if (!link || !shown.connected || !shown.request || shown.waiting || shown != currentLimit()) return;
        sync::Action action;
        if (choice == "account") action.aiName = shown.account;
        else if (choice == "model") action.aiName = shown.model;
        if (action.aiName.empty()) return;
        action.kind = sync::ActionKind::ResolveLimit;
        action.session = shown.session;
        action.request = shown.request;
        action.account = shown.owner;
        action.text = choice;
        limitSelections_[shown.session] = {shown.request, link->send(std::move(action))};
    };
    view->accountAction = [this](const std::string& action) {
        if (action == "dismiss" || action == "reveal") {
            const auto account = view->signInAccount();
            view->accountPicker.reset();
            view->dismissedSignIn.reset();
            if (action == "dismiss" && !account.name.empty())
                view->dismissedSignIn = std::make_pair(model->tab(model->active()).id, account.name);
            return;
        }
        auto picker = previewAccounts(0);
        if (!picker) return;
        const auto colon = action.find(':');
        const auto op = action.substr(0, colon);
        const auto target = action.substr(colon + 1);
        const auto option = std::find_if(picker->options.begin(), picker->options.end(),
            [&](const auto& item) { return item.name == target; });
        if (option == picker->options.end()) return;
        view->dismissedSignIn.reset();
        if (op == "focus") {
            picker->highlighted = static_cast<std::size_t>(option - picker->options.begin());
            view->accountPicker = std::move(picker);
        } else if (op == "select" || op == "sign_in") {
            view->chirp(view->windowApp.surface.confirmChirp);
            view->accountPicker.reset();
            if (op == "sign_in") startLogin(target);
            else if (target != picker->current) setAccount(picker->session, target);
        }
    };
    view->previewAccounts = [this](int direction) { return previewAccounts(direction); };
    wireAccountMenu();
    view->pickerCommand = [this](const std::string& command) { pickerCommand(command); };
    view->chooseAccount = [this](const std::string& account) { chooseAccount(account); };
    view->dismissAI = [this](const Tab& tab) {
        strip.dismiss(tab);
        mirror.forget(tab.id);
        if (!tab.draft.empty() && keep && keptStore && keptServer(host)) {
            std::string error;
            if (!saveKeptChange(*keptStore, KeptChange{host, {{tab.name, tab.draft}}, {}}, &error))
                std::fprintf(stderr, "nexus: %s\n", error.c_str());
        }
    };
    view->activateSignIn = [this]() { return activateSignIn(); };
    view->clearOpen = [this](std::size_t tab) { mirror.clearOpen(tab); };
    view->sessionCapabilities = [this](std::string& modelName, std::string& effort, std::string& catalog) {
        if (!model->tab_count()) return;
        if (const auto* session = accountState_.find(mirror.session(model->tab(model->active()).id))) {
            modelName = session->info.model;
            effort = session->info.effort;
            catalog = session->info.catalog;
        }
    };
    view->signInAccount = [this]() -> SignInAccount {
        // A captured state shows the same card as the connected session. Activation requires a live link.
        if (phase != sync::LinkPhase::Connected || !model->tab_count() ||
            accountSelections_.contains(mirror.session(model->tab(model->active()).id))) return {};
        const auto* choice = activeChoice();
        if (!choice || (!choice->needsSignIn && !offerFor(choice->name))) return {};
        const auto* session = accountState_.find(mirror.session(model->tab(model->active()).id));
        return {choice->name, choice->label, choice->email, session->info.harness,
            std::string(model->tab(model->active()).displayName())};
    };
    view->accountCaption = [this]() -> std::string {
        if (model->tab_count() && model->tab(model->active()).primaryChat()) {
            const auto* choice = activeChoice();
            if (!choice) return {};
            return "Account · " + (choice->label.empty() ? choice->name : choice->label);
        }
        const auto* roster = activeAccountRoster();
        if (!roster) return {};
        const auto* session = accountState_.find(mirror.session(model->tab(model->active()).id));
        if (!session) return {};
        const auto& name = session->info.account;
        const auto choice = std::find_if(roster->accounts.begin(), roster->accounts.end(),
                                         [&](const auto& a) { return a.name == name; });
        const auto& label = choice != roster->accounts.end() && !choice->label.empty() ? choice->label : name;
        if (offersAuto(*roster) && session->info.autoAccount) {
            const auto& mode = view->windowApp.autoAccountLabel;
            return label.empty() ? "Account · " + mode : "Account · " + mode + " (" + label + ")";
        }
        return label.empty() ? "Account · None" : "Account · " + label;
    };
    view->growing = [this](std::size_t id) -> const std::string* {
        const sync::SessionEntry* open = mirror.open(id);
        return open != nullptr ? &open->text : nullptr;
    };
    view->activeAgents = [this](std::size_t id) { return mirror.activeAgents(id); };
    view->cursor = [this](std::size_t id) { return sync::cursorOf(phase, heardMsAgo, mirror, id); };
    view->goal = [this](std::size_t id) {
        const auto* session = accountState_.find(mirror.session(id));
        return session ? session->info.goal : std::string();
    };
    view->cancelGoal = [this] {
        if (!model->tab_count() || view->goal(model->tab(model->active()).id).empty()) return;
        if (!link || phase != sync::LinkPhase::Connected) {
            view->problem = "Reconnect to the server to stop this goal. It may still be running.";
            return;
        }
        sync::Action action;
        action.kind = sync::ActionKind::Send;
        action.session = mirror.session(model->tab(model->active()).id);
        action.text = "/goal off";
        link->send(std::move(action));
    };
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
            if (model->tab(i).kind != TabKind::AI || model->tab(i).name != service) continue;
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
        if (loginPreparation_) return;
        if (phase != sync::LinkPhase::Connected || !activeAccountRoster())
            return;
        const auto session = mirror.session(model->tab(model->active()).id);
        const auto* current = accountState_.find(session);
        if (!current) return;
        view->accountPicker.reset();
        setAccount(session, "+" + command);
    };
    view->signOut = [this] {
        if (loginPreparation_) {
            view->problem = "Finish or cancel the current account switch first.";
            return false;
        }
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
        if (view->accountPicker || accountSelections_.contains(session)) {
            view->problem = "Finish selecting the account before signing out.";
            return false;
        }
        sync::Action action;
        action.kind = sync::ActionKind::SignOut;
        action.session = session;
        action.account = choice->name;
        const auto number = link->send(std::move(action));
        if (accountState_.nextAction()) accountSelections_[session] = number;
        view->problem.clear();
        return true;
    };
    view->submit = [this](std::size_t i, bool queue) {
        if (i < model->tab_count() && model->tab(i).draft == "/logout") {
            view->problem = "Use Enter in the AI prompt to sign out.";
            return;
        }
        const auto* current = accountState_.find(sync::sessionOf(*model, i, mirror));
        const bool command = i == model->active() && view->windowApp.surface.promptCommand;
        if (!command && current && current->info.limitRequest) return;
        if (!command && i == model->active() && activateSignIn()) return;
        if (upload_.valid()) {
            view->problem = "An attachment is uploading. Send this bubble when it finishes.";
            return;
        }
        std::string text;
        std::string why;
        // The session the tab shows, the window's own choice; none until the server's state has one of the AI's
        // (the window ensured one as the tab was added), and until then the draft stays.
        const sync::SessionId session = sync::sessionOf(*model, i, mirror);
        if (session == 0 && i < model->tab_count() && model->tab(i).kind == TabKind::AI) {
            why = "No session with " + std::string(model->tab(i).displayName()) + " yet: the prompt is kept.";
        } else if (sync::takePrompt(*model, i, sync::Limits{}.text, &text, &why)) {
            view->problem.clear();
            sync::Action action;
            action.kind = queue ? sync::ActionKind::Queue : sync::ActionKind::Send;
            action.session = session;
            action.text = std::move(text);
            std::vector<StagedFile> files;
            const auto tabId = model->tab(i).id;
            for (const auto& file : stagedFiles_)
                if (file.tab == tabId && action.text.find(file.marker) != std::string::npos) files.push_back(file);
            if (files.empty()) {
                if (!command && accountState_.nextAction()) {
                    auto shown = action.text;
                    const auto number = link->send(std::move(action));
                    view->outgoing.push_back({tabId, number, std::move(shown)});
                } else link->send(std::move(action));
                return;
            }
#if NEXUS_PROCESSES
            const auto aiName = model->tab(i).name;
            const auto instance = current ? current->info.instance : "";
            view->outgoing.push_back({tabId, 0, action.text});
            std::promise<UploadResult> ready;
            upload_ = ready.get_future();
            uploadWorker_ = std::async(std::launch::async, [action = std::move(action), files = std::move(files),
                    target = host, tabId, aiName, instance, wake = uploadWake, ready = std::move(ready)]() mutable {
                UploadResult result{std::move(action), tabId, {}, {}};
                result.original = result.action.text;
                for (const auto& file : files) {
                    const std::string header = aiName + "\n" + instance + "\n" + file.name + "\n" +
                        std::to_string(file.bytes->size()) + "\n";
                    Command command{"ssh", {"-T", "-o", "BatchMode=yes", "--", target, "exec \"$HOME/.local/bin/nexus-receive\""}, "attachment upload"};
                    auto reply = runCommandWithInput(command, 120, header + *file.bytes);
                    if (!reply.out.empty() && reply.out.back() == '\n') reply.out.pop_back();
                    if (reply.end != CommandResult::End::Exited || reply.code || !reply.out.starts_with("incoming/") ||
                        !reply.out.ends_with("/" + file.name) || reply.out.find('\n') != std::string::npos) {
                        result.error = "Attachment upload failed for " + file.name + ". " +
                            (reply.err.empty() ? "Check the server connection and nexus-receive installation." : reply.err);
                        break;
                    }
                    // Replace only the original composer markers. The remote path preserves the basename.
                    for (size_t pos = 0; (pos = result.action.text.find(file.marker, pos)) != std::string::npos;) {
                        result.action.text.replace(pos, file.marker.size(), reply.out);
                        pos += reply.out.size();
                    }
                }
                if (result.action.text.size() > kPromptBytes) result.error = "Attachment paths exceed the prompt limit.";
                ready.set_value(std::move(result));
                if (wake) wake();
            });
            return;
#else
            // Unreachable: this build stages no file (stageFile), so no prompt names one.
            (void)current;
            link->send(std::move(action));
            return;
#endif
        }
        if (!why.empty())
            view->problem = why;
    };
    // Command-Escape: a stop of the session the tab shows, sent whatever the window shows and whether or not the link
    // is up (it waits in order with the other actions); the server decides what it stops. Before any state has held a
    // session of the AI there is no session to name, and nothing is sent.
    view->stop = [this] {
        sync::SessionId session = 0;
        if (sync::stopTarget(*model, model->active(), mirror, &session)) {
            sync::Action action;
            action.kind = sync::ActionKind::Stop;
            action.session = session;
            link->send(std::move(action));
        }
    };
    // The AI tabs, and a session with each, come with the server's state (takeAIs).
    view->notice.clear();
    wire();
}

void FrameHost::showState(const sync::Sessions& state, bool lost) {
    accountState_ = state;
    takeAIs(state);
    mirror.apply(*model, state);
    phase = sync::LinkPhase::Connected;
    refreshPicker();
    heardMsAgo = lost ? sync::kQuietMs : 0;
    wire();
}

void FrameHost::pollBackground() {
    if (!view || !model) return;
    pollLink();
    prepareLogin();
    if (loginPreparation_ && loginPreparation_->machine.phase() == LoginPreparation::Phase::Browser &&
            view->accounts.dialog != AccountMenuView::Dialog::BrowserLogout && view->cancelLoginPreparation)
        view->cancelLoginPreparation();
    if (signIn.activeAccount) {
        const auto target = signInTarget();
        if (!target) signIn.activeAccount->clear();
        else if (*signIn.activeAccount != *target) *signIn.activeAccount = *target;
    }
    if (view->lockScreen && !signIn.locked) signIn.lock();
    else if (!view->lockScreen) signIn.unlock();
    signIn.step();
    if (!signIn.problem.empty()) {
        view->problem = std::move(signIn.problem);
        signIn.problem.clear();
    }
}

void FrameHost::beginFrame(PrKit* kit, PrFontSet* fonts, double time, const PrAppDisplay& display) {
    // Studio defaults to separate desktop windows for dialogs. An overlay must keep them in its own window:
    // a native child takes the keyboard, which correctly triggers the overlay's hide-on-focus-loss rule.
    // Prax still owns modal focus, Escape, and input routing; the client lock covers the whole surface.
    if (view && view->look && view->look->overlay) prUiSetHost(kit->ui, nullptr);
    ++pasteFrame_;
    // Another tab chosen: its prompt takes the focus, as a switch does (View::promptDismissed).
    if (view->promptDismissed && (model->tab_count() == 0 || model->tab(model->active()).id != tapTab_))
        view->promptDismissed = false;
    if (!upload_.valid()) std::erase_if(stagedFiles_, [&](const StagedFile& file) {
        if (pasteFrame_ - file.frame < 3) return false;
        for (size_t i = 0; i < model->tab_count(); ++i) {
            const auto& tab = model->tab(i);
            if (tab.id == file.tab && (tab.draft.find(file.marker) != std::string::npos ||
                tab.held.find(file.marker) != std::string::npos)) return false;
        }
        return true;
    });
    view->hitch = hitch;
#if NEXUS_PROCESSES
    if (terminals != nullptr) {
        // Each terminal's foreground process, for its tab's label, read where its working directory is read.
        HitchScope scope(hitch, Phase::Terminals);
        terminals->observe(time);
        if (ui != nullptr) {
            terminals->deliver(ui);
        }
    }
#endif
    {
        HitchScope scope(hitch, Phase::Keep);
        keepTerminals();
    }
    view->time = time;
    {
        HitchScope scope(hitch, Phase::SyncApply);
        pollBackground();
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

bool FrameHost::stageFile(std::string name, std::string bytes) {
    // Nothing is staged while locked (View::lockScreen): a clipboard file or image read that finishes after the lock
    // starts, before the next frame takes the prompt away, lands nowhere.
    if (view->lockScreen) return false;
    if (!ui || view->prompt == kPrLayoutNodeNone || prUiFocus(ui) != view->prompt ||
        (view->secrets && view->secrets->showing()) || !model->tab_count()) return false;
#if !NEXUS_PROCESSES
    // An attachment goes to the server through a child process (ssh running nexus-receive, the upload worker above),
    // which this build cannot start (build_parts.h). The in-app ssh (link/inapp_ssh.h) runs only the link's commands.
    (void)bytes;
    view->problem = "Attaching " + name + " is not available on iOS yet; nothing was sent.";
    return false;
#endif
    if (name.empty() || name == "." || name == ".." || name.size() > 255 ||
        name.find_first_of("/\\") != std::string::npos || std::any_of(name.begin(), name.end(),
            [](unsigned char c) { return c < 32 || c == 127; })) {
        view->problem = "Cannot attach a file with this filename.";
        return false;
    }
    size_t total = bytes.size();
    for (const auto& file : stagedFiles_) total += file.bytes->size();
    if (bytes.size() > kMaxAttachmentBytes || total > 4 * kMaxAttachmentBytes) {
        view->problem = "Attachments support 32 MiB per file and 128 MiB staged at once.";
        return false;
    }
    const auto marker = "[" + (name == "pasted.txt" ? "Pasted" : name) + " (" +
        std::to_string(bytes.size()) + " bytes) #" + std::to_string(++stagedCount_) + "]";
    if (marker.size() > promptRoom(*view, ui)) {
        view->problem = "The prompt has no room for another attachment.";
        return false;
    }
    stagedFiles_.push_back({model->tab(model->active()).id, pasteFrame_, marker, std::move(name),
        std::make_shared<const std::string>(std::move(bytes))});
    PrAppEvent event{};
    event.kind = PrAppEventKind_Paste;
    event.input.text = {marker.data(), marker.size()};
    prUiPushEvent(ui, kPrUiViewportMain, &event);
    return true;
}

bool isInputEvent(const PrAppEvent& e) {
    switch (e.kind) {
    case PrAppEventKind_PointerMove:
    case PrAppEventKind_PointerDown:
    case PrAppEventKind_PointerUp:
    case PrAppEventKind_PointerCancel:
    case PrAppEventKind_PointerEnter:
    case PrAppEventKind_PointerLeave:
    case PrAppEventKind_Scroll:
    case PrAppEventKind_Gesture:
    case PrAppEventKind_TrackpadTouches:
    case PrAppEventKind_KeyDown:
    case PrAppEventKind_KeyUp:
    case PrAppEventKind_Text:
    case PrAppEventKind_Paste:
    case PrAppEventKind_CompositionUpdate:
    case PrAppEventKind_CompositionEnd:
    case PrAppEventKind_DragEnter:
    case PrAppEventKind_DragOver:
    case PrAppEventKind_DragLeave:
    case PrAppEventKind_Drop:
    case PrAppEventKind_DragEnd:
    case PrAppEventKind_DragPromise:
        return true;
    default:
        return false;
    }
}

bool lockScreenTakes(const PrAppEvent& e) {
    switch (e.kind) {
    case PrAppEventKind_PointerMove:
    case PrAppEventKind_PointerDown:
    case PrAppEventKind_PointerUp:
    case PrAppEventKind_PointerCancel:
    case PrAppEventKind_PointerEnter:
    case PrAppEventKind_PointerLeave:
        return true;
    case PrAppEventKind_KeyDown:
    case PrAppEventKind_KeyUp:
        return (e.modifiers & ~PrAppModifierFlag_CapsLock) == 0 &&
            (e.keyboard.key == PrAppKey_Enter || e.keyboard.key == PrAppKey_Escape);
    default:
        return false;
    }
}

bool resetsIdle(const PrAppEvent& e) {
    switch (e.kind) {
    case PrAppEventKind_KeyDown:
    case PrAppEventKind_Text:
    case PrAppEventKind_PointerDown:
    case PrAppEventKind_Scroll:
    case PrAppEventKind_Paste:
    case PrAppEventKind_Drop:
        return true;
    default:
        return false;
    }
}

namespace {

// How far a press may move and still be a tap, logical px: further, it is a drag or a scroll.
constexpr float kTapSlop = 10;

bool inside(PrLayoutTree* tree, PrLayoutNode node, PrVec2 p) {
    if (node == kPrLayoutNodeNone || !prLayoutNodeIsValid(tree, node)) return false;
    const PrRect r = prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

} // namespace

void FrameHost::watchTap(const PrAppEvent& e) {
    if (!view->tapEndsEditing || ui == nullptr) return;
    PrLayoutTree* tree = prUiTree(ui);
    switch (e.kind) {
    case PrAppEventKind_PointerDown:
        tapping_ = e.pointer.button == 0;
        tapDown_ = e.pointer.position;
        // A press in the prompt's box takes editing back up.
        if (inside(tree, view->promptBox, tapDown_)) view->promptDismissed = false;
        break;
    case PrAppEventKind_PointerMove:
        if (tapping_) {
            const float dx = e.pointer.position.x - tapDown_.x, dy = e.pointer.position.y - tapDown_.y;
            if (dx * dx + dy * dy > kTapSlop * kTapSlop) tapping_ = false;
        }
        break;
    case PrAppEventKind_PointerCancel:
    case PrAppEventKind_Scroll:
    case PrAppEventKind_Gesture:
        tapping_ = false;
        break;
    case PrAppEventKind_PointerUp: {
        if (!tapping_) break;
        tapping_ = false;
        if (inside(tree, view->promptBox, tapDown_)) break;
        for (const PrLayoutNode tab : view->tabNodes) {
            if (inside(tree, tab, tapDown_)) return;     // a tab switches, as it does
        }
        if (view->prompt != kPrLayoutNodeNone && prUiFocus(ui) == view->prompt) prUiSetFocus(ui, kPrLayoutNodeNone);
        view->promptDismissed = true;
        tapTab_ = model->tab_count() != 0 ? model->tab(model->active()).id : ~std::size_t(0);
        break;
    }
    case PrAppEventKind_KeyDown:
    case PrAppEventKind_Text:
    case PrAppEventKind_Paste:
        // A key typed (a hardware keyboard's) goes to the prompt again.
        if (view->promptDismissed) {
            view->promptDismissed = false;
            if (view->prompt != kPrLayoutNodeNone && prLayoutNodeIsValid(tree, view->prompt))
                prUiSetFocus(ui, view->prompt);
        }
        break;
    default:
        break;
    }
}

bool FrameHost::event(const PrAppEvent& e) {
    HitchScope scope(hitch, Phase::Input);
    // While locked (View::lockScreen) no input reaches anything but the lock screen: no key reaches a terminal's shell,
    // the Dashboard's page or the prompt, and nothing is pasted or dropped. What is not input (a resize, a timer) passes.
    if (view->lockScreen && isInputEvent(e)) return !lockScreenTakes(e);
    // A modal owns all input, including mouse presses over transcript text underneath it.
    // Let Prax route it before the transcript selection, composer, board or terminal can consume anything.
    if (view->accounts.dialog != AccountMenuView::Dialog::None) return false;
    watchTap(e);
    // While the secret dialog shows, every event is the window's tree's, where the dialog's field has the focus: none
    // reaches the sprint tab's page or a terminal's shell, so a value typed or pasted goes only to the field.
    if (view->secrets != nullptr && view->secrets->showing() != nullptr) {
        return false;
    }
    // The account menu under the active AI's tab takes its press, and every pointer event of that gesture.
    if (routeAccountMenu(*view, ui, e)) return true;
    if (!view->accountPicker && view->choiceKind.empty() &&
            (!view->windowApp.surface.items || view->windowApp.surface.items->empty())) {
        const bool output = view->selection.event(e, view->transcript, view->copy, view->openLink);
        if (routePromptPointer(*view, ui, e) || output) return true;
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
#if NEXUS_PROCESSES
    // Typed into a terminal tab before its screen can take it: held for its shell, in order.
    if (terminals != nullptr && model->tab_count() != 0) {
        const Tab& tab = model->tab(model->active());
        if (tab.kind == TabKind::Terminal && terminals->hold(tab.id, e)) {
            return true;
        }
    }
#endif
    if (ui == nullptr || (e.kind != PrAppEventKind_Text && e.kind != PrAppEventKind_Paste) ||
            view->prompt == kPrLayoutNodeNone || prUiFocus(ui) != view->prompt) {
        return false;
    }
    // Text that fits the room left (the selection it replaces counted) goes on as it came.
    if (e.kind == PrAppEventKind_Paste && e.input.text.size >= pastePreviewBytes) {
        if (e.input.text.size > kMaxAttachmentBytes) view->problem = "The paste exceeds the 32 MiB attachment limit.";
        else stageFile("pasted.txt", std::string(e.input.text.data, e.input.text.size));
        return true;
    }
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

namespace {

// The box the window is built in above the on-screen keyboard (FrameHost::buildAbove).
struct AboveKeyboard {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;
    float height;

    void make(const View&, PrLayoutBoxStyle& box) const {
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.display = PrLayoutDisplay_Flex;
        box.width = prLayoutLengthPx(width);
        box.height = prLayoutLengthPx(height);
    }
};

} // namespace

void FrameHost::buildAbove(PrKit* kit, PrLayoutNode root, PrVec2 box) {
    view->extent = box;
    const PrLayoutNode above = prUiElement(ui, root, prHashString(PrStringView{"above-keyboard", 14}, 0),
        pr::uiStyle(ui, *view, AboveKeyboard{view->extent.x, view->extent.y}), 0);
    build(kit, above);
}

} // namespace nexus
