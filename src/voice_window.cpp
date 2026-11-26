#include "voice_window.h"

#include <utility>

#include "cursor.h"

namespace nexus {
VoiceWindow::VoiceWindow(Model& model, View& view)
    : model_(model), view_(view),
      host_(nullptr, voice::VoiceSurface{
          [this](const std::string& name) {
              // From the menu the prompt is answered on the menu (panel_.to): the tab opens only from a tab.
              if (view_.phoneMenu) return;
              const auto i = aiIndex(name);
              if (i < model_.tab_count()) model_.select_tab(i);
          },
          [this](const std::string& name, const std::string& text) { send(name, text); },
          [this](const std::string& name, const std::string& text) { draft(name, text); },
      }) {
    panel_.press = [this] { host_.press(now_); };
    panel_.openTo = [this] {
        const auto i = aiIndex(panel_.to);
        if (i >= model_.tab_count()) return;
        model_.select_tab(i);
        view_.phoneMenu = false;
    };
    view_.voice = &panel_;
}

VoiceWindow::~VoiceWindow() {
    if (view_.voice == &panel_) view_.voice = nullptr;
}

void VoiceWindow::setSpeech(std::unique_ptr<voice::Speech> speech) {
    speech_ = std::move(speech);
    host_.setSpeech(speech_.get());
}

std::size_t VoiceWindow::aiIndex(const std::string& name) const {
    for (std::size_t i = 0; i < model_.tab_count(); ++i)
        if (model_.tab(i).kind == TabKind::Friend && model_.tab(i).name == name) return i;
    return model_.tab_count();
}

// As a typed prompt is sent: the text in the tab's draft, then the window's submit (the link's, which takes the draft);
// a draft the person had typed there is put back after. Without a server (no submit) the model sends it locally. A
// prompt the window could not send stays in the draft, with what was typed before it: nothing is lost.
void VoiceWindow::send(const std::string& name, const std::string& text) {
    if (!view_.phoneMenu) {
        // A tab's microphone is dictation: the words go into its prompt, after what was typed, to edit and send.
        // Never sent from here, even when the listen began and ended between two frames.
        beginDictation();
        endDictation(text);
        return;
    }
    const auto i = aiIndex(name);
    if (i >= model_.tab_count()) return;
    const std::string typed = model_.tab(i).draft;
    busy_[model_.tab(i).id] = false; // only a turn that ends after this send ends its wait for a reply
    if (view_.phoneMenu) {
        // Sent from the menu: the menu shows who it went to and the reply as it comes.
        panel_.to = name;
        panel_.replyText.clear();
    }
    model_.set_draft(i, text);
    if (view_.submit) view_.submit(i, false);
    else model_.send(i);
    const std::string left = model_.tab(i).draft;
    if (left.empty()) model_.set_draft(i, typed);
    else if (!typed.empty()) model_.set_draft(i, typed + "\n" + left);
}

void VoiceWindow::draft(const std::string& name, const std::string& text) {
    if (!view_.phoneMenu) {
        // A tab's microphone is dictation: the words go into its prompt, after what was typed, to edit and send.
        // Never sent from here, even when the listen began and ended between two frames.
        beginDictation();
        endDictation(text);
        return;
    }
    std::size_t i = name.empty() ? model_.active() : aiIndex(name);
    if (i >= model_.tab_count() || model_.tab(i).kind != TabKind::Friend) {
        i = model_.tab_count();
        for (std::size_t j = 0; j < model_.tab_count() && i == model_.tab_count(); ++j)
            if (model_.tab(j).kind == TabKind::Friend) i = j;
    }
    if (i >= model_.tab_count()) return; // no AI's tab at all: the voice panel's line still holds what was heard
    const std::string& typed = model_.tab(i).draft;
    model_.set_draft(i, typed.empty() ? text : typed + "\n" + text);
}

namespace {

std::string joined(const std::string& typed, const std::string& said) {
    if (typed.empty() || said.empty()) return typed + said;
    const char end = typed.back();
    return typed + (end == ' ' || end == '\n' ? "" : " ") + said;
}

} // namespace

void VoiceWindow::beginDictation() {
    if (dictating_ || model_.tab_count() == 0 || model_.active() >= model_.tab_count()) return;
    dictating_ = true;
    dictTab_ = model_.tab(model_.active()).id;
    dictBase_ = model_.tab(model_.active()).draft;
}

// The tab whose prompt is being dictated into gets what was typed there before, then the words.
void VoiceWindow::endDictation(const std::string& said) {
    if (!dictating_) return;
    dictating_ = false;
    const auto i = tabOf(dictTab_);
    if (i < model_.tab_count()) model_.set_draft(i, joined(dictBase_, said));
}

std::size_t VoiceWindow::tabOf(std::size_t id) const {
    for (std::size_t i = 0; i < model_.tab_count(); ++i)
        if (model_.tab(i).id == id) return i;
    return model_.tab_count();
}

void VoiceWindow::frame(double now) {
    now_ = now;
    // How replies come back is fixed by where the person is: the menu speaks, a tab texts (voice::replyOn).
    const auto reply = voice::replyOn(view_.phoneMenu);
    if (host_.turn().reply() != reply) host_.setReply(reply);
    // The AI showing: a tab's own; on the menu none, so a prompt naming no one goes to the first AI (VoiceTurn). In a
    // tab the microphone is plain dictation into its prompt: no names are matched (no AIs given), nothing is spoken.
    std::string current;
    std::vector<voice::SpokenAI> ais;
    const auto& names = view_.windowApp.surface.voiceNames;
    for (std::size_t i = 0; i < model_.tab_count(); ++i) {
        const auto& tab = model_.tab(i);
        if (tab.kind != TabKind::Friend || tab.primaryChat()) continue;
        const auto aliases = names.find(tab.name);
        ais.push_back({tab.name, aliases != names.end() ? aliases->second : std::vector<std::string>{}});
        if (i == model_.active() && !view_.phoneMenu) current = tab.name;
    }
    if (!view_.phoneMenu) ais.clear();
    host_.frame(now, std::move(current), std::move(ais));
    const auto& phase = host_.turn().phase();
    const bool listening = phase == voice::Phase::Listening || phase == voice::Phase::Finishing;
    if (listening && !view_.phoneMenu) beginDictation();
    if (dictating_) {
        if (listening) {
            const auto i = tabOf(dictTab_);
            if (i < model_.tab_count()) model_.set_draft(i, joined(dictBase_, host_.turn().heardText()));
        } else {
            endDictation(""); // heard nothing: the prompt as it was
        }
    }

    // Each AI's reply, a block at a time as it closes into the transcript; and the end of its turn, from the state
    // cursor (the server's state: waiting for the person again). What a tab held when first seen is history, not a reply.
    for (std::size_t i = 0; i < model_.tab_count(); ++i) {
        const auto& tab = model_.tab(i);
        if (tab.kind != TabKind::Friend) continue;
        auto [seen, fresh] = seen_.try_emplace(tab.id, tab.transcript.size());
        if (!fresh && tab.transcript.size() < seen->second) seen->second = tab.transcript.size(); // cleared
        for (; seen->second < tab.transcript.size(); ++seen->second) {
            const auto& entry = tab.transcript[seen->second];
            if (entry.kind != EntryKind::Friend) continue;
            host_.replied(tab.name, voice::speakable(entry.text));
            if (tab.name == panel_.to) panel_.replyText += (panel_.replyText.empty() ? "" : "\n\n") + entry.text;
        }
        if (view_.cursor) {
            const auto state = view_.cursor(tab.id);
            const bool open = state == CursorState::Busy || state == CursorState::Working;
            bool& was = busy_[tab.id];
            if (was && !open) host_.turnEnded(tab.name);
            was = open;
        }
    }

    const auto& turn = host_.turn();
    panel_.listening = turn.phase() == voice::Phase::Listening || turn.phase() == voice::Phase::Finishing;
    panel_.asking = turn.phase() == voice::Phase::Asking;
    panel_.speaking = turn.speaking();
    panel_.heard = turn.heardText();
    panel_.line = turn.line();
    panel_.trouble = turn.trouble();
    const auto to = aiIndex(panel_.to);
    const auto& titles = view_.windowApp.surface.tabTitles;
    panel_.toTitle = to < titles.size() ? titles[to] : panel_.to;
}

} // namespace nexus
