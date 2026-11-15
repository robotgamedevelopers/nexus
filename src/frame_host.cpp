#include "frame_host.h"

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
    if (text != savedTerminals) {
        savedTerminals = text;
        std::string error;
        if (!saveTerminals(*terminalStore, directories, &error))
            std::fprintf(stderr, "nexus: %s\n", error.c_str());
    }
    keepSelection();
}

void FrameHost::personSelected() {
    strip.personSelected();
    keepSelection();
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

void FrameHost::pollLink() {
    LinkState s;
    if (link == nullptr || !link->poll(&s))
        return;
    // The friends first, so that a friend's new tab shows its session in this same state.
    if (s.phase == sync::LinkPhase::Connected) {
        takeFriends(s.sessions);
    }
    mirror.apply(*model, s.sessions);
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

void FrameHost::wire() {
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
    view->promptOpen = false;
    view->status = sync::linkStatus(sync::LinkPhase::Connecting, host, 0, "");
    view->submit = [this](std::size_t i, bool queue) {
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
    takeFriends(state);
    mirror.apply(*model, state);
    phase = sync::LinkPhase::Connected;
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
