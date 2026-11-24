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

enum class WindowCommand { SelectTab, MoveTab, Zoom, OpenTerminal, CloseTerminal, Stop, OpenTab, CloseTab, InvokeService, AccountCommand, AccountPicker, AccountAction, ResolveLimit };
struct Shortcut {
    PrAppKey key;
    PrAppModifierFlags modifiers;
    WindowCommand command;
    int argument;
    std::string type{};
};
enum class Panel { Board, Terminal, Transcript, Prompt, Status, Account, LimitChoice, Native, TextArea, Label, ServiceReply };
struct TabType {
    std::string title, group, native, service;
    bool operator==(const TabType&) const = default;
};

struct AccountItem {
    std::string text, action;
    bool selected = false;
};

struct AccountSurface {
    std::optional<std::vector<AccountItem>> items;
    std::vector<std::string> tabTitles;
    bool showTabs = true, reorderTabs = true;
};

struct MessageCardStyle {
    unsigned fill = 0xffffff;
    unsigned ink = 0;
    unsigned emailInk = 0x477fba;
    float textSize = 34;
    float radius = 20;
    float sideExtension = 0;
    float rowHeight = 1.45f, rowGap = 0.15f, headingGap = 0.8f;
    float emailWeight = 700;
    unsigned selectedFill = 0x86c5ff, selectedInk = 0xffffff;
    bool operator==(const MessageCardStyle&) const = default;
};

struct TranscriptStyle {
    unsigned fill = 0x252525;
    unsigned ink = 0xf3f3f3;
    unsigned personInk = ink;
    unsigned switchEmailInk = 0x86c5ff;
    unsigned noticeInk = 0xffb4b4;
    unsigned statusInk = 0xd8d8d8;
    float radius = style::kBubbleRadius;
    bool showNotices = true;
    bool showTools = true;
    bool operator==(const TranscriptStyle&) const = default;
};

struct SignInAccount {
    std::string name, label, email, provider{}, friendName{};
    bool operator==(const SignInAccount&) const = default;
};

struct LimitOffer {
    uint64_t session = 0, request = 0;
    std::string owner{}, account{}, model{};
    bool waiting = false, connected = false;
    bool operator==(const LimitOffer&) const = default;
};

class WindowApplication {
public:
    void update(policy::Host& host, const char* tab, const std::string& type, bool promptOpen, bool hasStatus,
                std::size_t activeTab, std::size_t tabCount, const std::string& group = {}, SignInAccount account = {},
                bool pickerOpen = false, LimitOffer offer = {}, const std::string& ui = "{}");
    std::vector<Shortcut> shortcuts;
    std::vector<Panel> panels;
    std::map<std::string, TabType> types;
    std::vector<TabGroup> groups;
    std::optional<bool> closable;
    uint64_t layoutGeneration = 0;
    std::string label;
    std::string autoAccountLabel = "auto";
    std::string signInHeading = "Please sign in";
    bool centeredAccounts = false;
    bool signInOnSelect = false;
    AccountSurface surface;
    SignInAccount signInAccount;
    LimitOffer limitOffer;
    std::string limitAccountLabel, limitModelLabel;
    MessageCardStyle messageCard;
    TranscriptStyle transcriptStyle;
    float overlayOpacity = style::kOverlayOpacity;

private:
    std::string lastContext_;
    uint64_t generation_ = 0;
    std::optional<uint64_t> appliedGeneration_;
};

} // namespace nexus
