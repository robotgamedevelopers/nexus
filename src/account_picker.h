// Local preview of a server session's linked accounts. Committing it is a separate link action.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sync/sessions.h"

namespace nexus {

// A menu row is a selection target, which can be a mode or the name of a linked account.
struct PickerOption {
    std::string name;
    std::string label;
};

inline bool offersAuto(const sync::ServerFriend& roster) {
    return roster.canAutoAccount && roster.accounts.size() > 1;
}

struct AccountPicker {
    std::size_t tab;
    uint64_t session;
    std::vector<PickerOption> options;
    std::size_t highlighted;
    std::string current;  // selected row: a pinned account or automatic mode
    std::string account;  // concrete account, including while "auto" is selected

    static std::vector<PickerOption> rows(const sync::ServerFriend& roster) {
        std::vector<PickerOption> result;
        const bool automatic = offersAuto(roster);
        if (automatic) result.push_back({sync::kAutoAccountTarget, {}});
        for (const auto& a : roster.accounts) {
            if (a.name.empty() || a.name == sync::kAutoAccountTarget || a.name.front() == '+') continue;
            const auto& label = a.label.empty() ? a.name : a.label;
            result.push_back({a.name, label});
        }
        return result;
    }

    std::string rowLabel(const PickerOption& row, const std::string& autoLabel) const {
        if (row.name == sync::kAutoAccountTarget) return autoLabel;
        if (options.front().name == sync::kAutoAccountTarget && row.label == autoLabel)
            return row.label + " (account)";
        return row.label;
    }

    static std::optional<AccountPicker> open(std::size_t tab, const sync::SessionView& session,
                                             const sync::ServerFriend& roster, int direction) {
        auto options = rows(roster);
        if (options.empty()) return std::nullopt;
        const bool automatic = offersAuto(roster) && session.info.autoAccount;
        AccountPicker picker{tab, session.id, std::move(options), 0,
                             automatic ? sync::kAutoAccountTarget : session.info.account, session.info.account};
        const auto it = std::find_if(picker.options.begin(), picker.options.end(), [&](const auto& a) { return a.name == picker.current; });
        if (it == picker.options.end())
            picker.highlighted = direction < 0 ? picker.options.size() - 1 : 0;
        else {
            picker.highlighted = static_cast<std::size_t>(it - picker.options.begin());
            picker.step(direction);
        }
        return picker;
    }

    void step(int direction) {
        if (direction > 0)
            highlighted = (highlighted + 1) % options.size();
        else if (direction < 0)
            highlighted = highlighted ? highlighted - 1 : options.size() - 1;
    }

    // Keep the preview attached to its account id when the server changes list order or display metadata.
    bool refresh(std::size_t activeTab, const sync::SessionView& activeSession, const sync::ServerFriend& roster) {
        const bool automatic = offersAuto(roster) && activeSession.info.autoAccount;
        if (activeTab != tab || activeSession.id != session || activeSession.info.account != account ||
                automatic != (current == sync::kAutoAccountTarget)) return false;
        auto choices = rows(roster);
        const auto& name = options[highlighted].name;
        const auto it = std::find_if(choices.begin(), choices.end(), [&](const auto& a) { return a.name == name; });
        if (it == choices.end()) return false;
        highlighted = static_cast<std::size_t>(it - choices.begin());
        options = std::move(choices);
        return true;
    }

    std::optional<std::string> choice(const std::string& name) const {
        if (name == current ||
                std::none_of(options.begin(), options.end(), [&](const auto& a) { return a.name == name; }))
            return std::nullopt;
        return name;
    }
};

} // namespace nexus
