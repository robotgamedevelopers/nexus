// Local preview of a server session's linked accounts. Committing it is a separate link action.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sync/sessions.h"

namespace nexus {

using PickerAccount = sync::AccountChoice;

struct AccountPicker {
    std::size_t tab;
    uint64_t session;
    std::vector<PickerAccount> options;
    std::size_t highlighted;
    std::string current;

    static AccountPicker open(std::size_t tab, uint64_t session, const std::vector<PickerAccount>& options,
                              const std::string& current, int direction) {
        assert(!options.empty());
        AccountPicker picker{tab, session, options, 0, current};
        const auto it = std::find_if(options.begin(), options.end(), [&](const auto& a) { return a.name == current; });
        if (it == options.end())
            picker.highlighted = direction < 0 ? options.size() - 1 : 0;
        else {
            picker.highlighted = static_cast<std::size_t>(it - options.begin());
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
    bool refresh(std::size_t activeTab, uint64_t activeSession, const std::vector<PickerAccount>& choices,
                 const std::string& activeAccount) {
        if (activeTab != tab || activeSession != session || activeAccount != current) return false;
        const auto& name = options[highlighted].name;
        const auto it = std::find_if(choices.begin(), choices.end(), [&](const auto& a) { return a.name == name; });
        if (it == choices.end()) return false;
        highlighted = static_cast<std::size_t>(it - choices.begin());
        options = choices;
        return true;
    }

    std::optional<std::string> choice(const std::string& name) const {
        // The wire reserves an empty target for clearing the account and "+" for relative commands.
        if (name.empty() || name.front() == '+' || name == current ||
                std::none_of(options.begin(), options.end(), [&](const auto& a) { return a.name == name; }))
            return std::nullopt;
        return name;
    }
};

} // namespace nexus
