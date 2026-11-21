// A Starlark window description compiled into native key captures and ordered panel draws.
#pragma once

#include <map>
#include <vector>

#include "policy/host.h"
#include "pr/app_input.h"

namespace nexus {

enum class WindowCommand { SelectTab, Zoom, OpenTerminal, CloseTerminal, Stop, OpenTab, CloseTab, InvokeService };
struct Shortcut {
    PrAppKey key;
    PrAppModifierFlags modifiers;
    WindowCommand command;
    int argument;
    std::string type{};
};
enum class Panel { Board, Terminal, Transcript, Prompt, Status, Native, TextArea, Label, ServiceReply };
struct TabType { std::string title, group, native, service; };

class WindowApplication {
public:
    void update(policy::Host& host, const char* tab, const std::string& type, bool promptOpen, bool hasStatus);
    std::vector<Shortcut> shortcuts;
    std::vector<Panel> panels;
    std::map<std::string, TabType> types;
    std::string label;

private:
    std::string lastContext_;
    uint64_t generation_ = 0;
};

} // namespace nexus
