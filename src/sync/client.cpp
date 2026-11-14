#include "sync/client.h"

#include <algorithm>
#include <cctype>

namespace nexus::sync {

bool blankText(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
        [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; });
}

std::string linkStatus(LinkPhase phase, const std::string& host, std::uint64_t heardMsAgo, const std::string& problem) {
    switch (phase) {
        case LinkPhase::Connecting:
            return "Connecting to " + host;
        case LinkPhase::Failed:
            return host + ": " + problem;
        case LinkPhase::Connected:
            if (heardMsAgo < kQuietMs)
                return std::string();
            return "No contact with " + host + " for " + std::to_string(heardMsAgo / 1000) + " s";
    }
    return std::string();
}

bool linkLost(LinkPhase phase, std::uint64_t heardMsAgo) {
    return phase != LinkPhase::Connected || heardMsAgo >= kQuietMs;
}

CursorState cursorOf(LinkPhase phase, std::uint64_t heardMsAgo, const Mirror& mirror, std::size_t tabId) {
    if (linkLost(phase, heardMsAgo)) {
        return CursorState::Lost;
    }
    const Activity* activity = mirror.activity(tabId);
    if (activity == nullptr) {
        return CursorState::Lost;
    }
    switch (*activity) {
        case Activity::Busy:
            return CursorState::Busy;
        case Activity::Down:
            return CursorState::Down;
        case Activity::Idle:
            break;
    }
    return CursorState::Idle;
}

bool takePrompt(Model& model, std::size_t i, std::size_t limit, std::string* text, std::string* why) {
    if (i >= model.tab_count() || model.tab(i).kind != TabKind::Friend)
        return false;
    const std::string& draft = model.tab(i).draft;
    if (blankText(draft))
        return false;
    if (draft.size() > limit) {
        if (why != nullptr)
            *why = "This prompt is " + std::to_string((draft.size() + 1023) / 1024) + " KiB; the limit is " +
                std::to_string(limit / 1024) + " KiB.";
        return false;
    }
    *text = draft;
    model.set_draft(i, std::string());
    return true;
}

SessionId sessionOf(const Model& model, std::size_t i, const Mirror& mirror) {
    if (i >= model.tab_count() || model.tab(i).kind != TabKind::Friend) {
        return 0;
    }
    return mirror.session(model.tab(i).id);
}

std::vector<std::string> friendsToEnsure(const Model& model) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::Friend) {
            out.push_back(model.tab(i).name);
        }
    }
    return out;
}

bool stopTarget(const Model& model, std::size_t i, const Mirror& mirror, SessionId* session) {
    const SessionId id = sessionOf(model, i, mirror);
    if (id == 0) {
        return false;
    }
    *session = id;
    return true;
}

} // namespace nexus::sync
