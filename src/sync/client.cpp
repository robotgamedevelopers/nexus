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

bool stopTarget(const Model& model, std::size_t i, std::string* tab) {
    if (i >= model.tab_count() || model.tab(i).kind != TabKind::Friend) {
        return false;
    }
    *tab = model.tab(i).name;
    return true;
}

} // namespace nexus::sync
