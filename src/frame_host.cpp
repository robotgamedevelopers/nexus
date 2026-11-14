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
    phase = s.phase;
    heardMsAgo = s.heardMsAgo;
    view->status = sync::linkStatus(s.phase, host, s.heardMsAgo, s.problem);
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
    }
    {
        HitchScope scope(hitch, Phase::Keep);
        keepTerminals();
    }
    view->time = time;
    {
        HitchScope scope(hitch, Phase::Input);
        pasteOn();
    }
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
    if (board != nullptr && board->event(e)) {
        return true;
    }
    if (ui == nullptr || (e.kind != PrAppEventKind_Text && e.kind != PrAppEventKind_Paste) ||
            view->prompt == kPrLayoutNodeNone || prUiFocus(ui) != view->prompt) {
        return false;
    }
    // Text typed while a paste is still going in follows it; so does a paste.
    const std::size_t free = promptRoom(*view);
    const std::size_t room = free > view->pasting.size() ? free - view->pasting.size() : 0;
    if (view->pasting.empty() && e.input.text.size <= room && e.input.text.size <= kPastePiece) {
        return false;
    }
    const PrStringView kept = cutText(e.input.text, room);
    if (kept.size != e.input.text.size) {
        view->problem = kPromptCut;
    }
    view->pasting.append(kept.data, kept.size);
    return true;
}

// The next piece of the pasted text, as a Paste event for the prompt, on its way to the tree for this frame. If the
// prompt lost the focus meanwhile (another tab shows), the rest is left out, and the transcript says so.
void FrameHost::pasteOn() {
    if (view->pasting.empty() || ui == nullptr) {
        return;
    }
    if (view->prompt == kPrLayoutNodeNone || prUiFocus(ui) != view->prompt) {
        view->pasting.clear();
        view->problem = kPromptCut;
        return;
    }
    const PrStringView piece = cutText(PrStringView{view->pasting.data(), view->pasting.size()}, kPastePiece);
    PrAppEvent e{};
    e.kind = PrAppEventKind_Paste;
    e.input.text = piece;
    prUiPushEvent(ui, kPrUiViewportMain, &e);
    view->pasting.erase(0, piece.size);
}

void FrameHost::build(PrKit* kit, PrLayoutNode root) {
    buildWindow(*view, kit, root);
}

} // namespace nexus
