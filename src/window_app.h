// A Starlark window description compiled into native key captures and ordered panel draws.
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "model.h"
#include "policy/host.h"
#include "style.h"
#include "pr/app_input.h"

namespace nexus {

enum class WindowCommand { SelectTab, MoveTab, Zoom, OpenTerminal, CloseTerminal, Stop, OpenTab, CloseTab, InvokeService, AccountCommand };
struct Shortcut {
    PrAppKey key;
    PrAppModifierFlags modifiers;
    WindowCommand command;
    int argument;
    std::string type{};
};
enum class Panel { Board, Terminal, Transcript, Prompt, Status, Account, Native, TextArea, Label, ServiceReply };
struct TabType {
    std::string title, group, native, service;
    bool operator==(const TabType&) const = default;
};

struct MessageCardStyle {
    unsigned fill = 0xffffff;
    unsigned ink = 0;
    float textSize = 34;
    float radius = 20;
    bool operator==(const MessageCardStyle&) const = default;
};

class WindowApplication {
public:
    void update(policy::Host& host, const char* tab, const std::string& type, bool promptOpen, bool hasStatus,
                std::size_t activeTab, std::size_t tabCount, const std::string& group = {});
    std::vector<Shortcut> shortcuts;
    std::vector<Panel> panels;
    std::map<std::string, TabType> types;
    std::vector<TabGroup> groups;
    std::optional<bool> closable;
    uint64_t layoutGeneration = 0;
    std::string label;
    MessageCardStyle messageCard;
    float overlayOpacity = style::kOverlayOpacity;

private:
    std::string lastContext_;
    uint64_t generation_ = 0;
    std::optional<uint64_t> appliedGeneration_;
};

} // namespace nexus
