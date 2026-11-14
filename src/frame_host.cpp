#include "frame_host.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "board.h"
#include "terminals.h"

static_assert(nexus::kPromptBytes == nexus::sync::Limits{}.text, "the prompt holds what the server takes");

namespace nexus {

void FrameHost::keepSelection() {
    if (!keep || selectionStore == nullptr)
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

void FrameHost::pollLink() {
    LinkState s;
    if (link == nullptr || !link->poll(&s))
        return;
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
        // (the window ensured one at launch), and then the draft stays.
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
    // A session with each friend the window shows: the server makes one only for a friend that has none.
    for (const std::string& name : sync::friendsToEnsure(*model)) {
        sync::Action ensure;
        ensure.kind = sync::ActionKind::Ensure;
        ensure.friendName = name;
        link->send(std::move(ensure));
    }
    wire();
}

void FrameHost::showState(const sync::Sessions& state, bool lost) {
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
