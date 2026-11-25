#include "voice_window.h"

#include <utility>

#include "cursor.h"

namespace nexus {
namespace {

voice::Reply replyOf(const std::string& name) {
    return name == "text" ? voice::Reply::Text : name == "voice" ? voice::Reply::Voice : voice::Reply::Both;
}

} // namespace

VoiceWindow::VoiceWindow(Model& model, View& view)
    : model_(model), view_(view),
      host_(nullptr, voice::VoiceSurface{
          [this](const std::string& name) {
              const auto i = aiIndex(name);
              if (i < model_.tab_count()) {
                  model_.select_tab(i);
                  view_.phoneMenu = false;
              }
          },
          [this](const std::string& name, const std::string& text) { send(name, text); },
          [this](const std::string& name, const std::string& text) { draft(name, text); },
      }) {
    panel_.press = [this] { host_.press(now_); };
    panel_.setReply = [this](const std::string& reply) {
        replyChosen_ = true;
        host_.setReply(replyOf(reply));
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
    const auto i = aiIndex(name);
    if (i >= model_.tab_count()) return;
    const std::string typed = model_.tab(i).draft;
    busy_[model_.tab(i).id] = false; // only a turn that ends after this send ends its wait for a reply
    model_.set_draft(i, text);
    if (view_.submit) view_.submit(i, false);
    else model_.send(i);
    const std::string left = model_.tab(i).draft;
    if (left.empty()) model_.set_draft(i, typed);
    else if (!typed.empty()) model_.set_draft(i, typed + "\n" + left);
}

void VoiceWindow::draft(const std::string& name, const std::string& text) {
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

void VoiceWindow::frame(double now) {
    now_ = now;
    if (!replyChosen_) {
        const auto first = replyOf(view_.windowApp.surface.voiceReply);
        if (host_.turn().reply() != first) host_.setReply(first);
    }
    std::string current;
    std::vector<voice::SpokenAI> ais;
    const auto& names = view_.windowApp.surface.voiceNames;
    for (std::size_t i = 0; i < model_.tab_count(); ++i) {
        const auto& tab = model_.tab(i);
        if (tab.kind != TabKind::Friend || tab.primaryChat()) continue;
        const auto aliases = names.find(tab.name);
        ais.push_back({tab.name, aliases != names.end() ? aliases->second : std::vector<std::string>{}});
        if (i == model_.active()) current = tab.name;
    }
    host_.frame(now, std::move(current), std::move(ais));

    // Each AI's reply, a block at a time as it closes into the transcript; and the end of its turn, from the state
    // cursor (the server's state: waiting for the person again). What a tab held when first seen is history, not a reply.
    for (std::size_t i = 0; i < model_.tab_count(); ++i) {
        const auto& tab = model_.tab(i);
        if (tab.kind != TabKind::Friend) continue;
        auto [seen, fresh] = seen_.try_emplace(tab.id, tab.transcript.size());
        if (!fresh && tab.transcript.size() < seen->second) seen->second = tab.transcript.size(); // cleared
        for (; seen->second < tab.transcript.size(); ++seen->second) {
            const auto& entry = tab.transcript[seen->second];
            if (entry.kind == EntryKind::Friend) host_.replied(tab.name, voice::speakable(entry.text));
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
    panel_.reply = voice::replyName(turn.reply());
}

} // namespace nexus
