#include "window_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string_view>

#include "pr/json.h"
#include "style.h"

namespace nexus {
namespace {
using Index = PrJsonValueIndex;

std::string_view stringAt(const PrJson* doc, Index i) {
    const auto s = prJsonString(doc, i);
    return s.data ? std::string_view(s.data, s.size) : std::string_view();
}

bool chord(std::string_view name, PrAppKey& key, PrAppModifierFlags& modifiers) {
    while (name.size() > 1 && name.find('+') != name.npos) {
        const auto n = name.find('+');
        const auto mod = name.substr(0, n);
        if (mod == "Command") modifiers |= style::kCommand;
        else if (mod == "Shift") modifiers |= PrAppModifierFlag_Shift;
        else if (mod == "Control") modifiers |= PrAppModifierFlag_Control;
        else if (mod == "Alt") modifiers |= PrAppModifierFlag_Alt;
        else if (mod == "Super") modifiers |= PrAppModifierFlag_Super;
        else return false;
        name.remove_prefix(n + 1);
    }
    if (name.size() == 1 && name.front() >= ' ' && name.front() <= '~') {
        key = static_cast<PrAppKey>(name.front());
        return true;
    }
    for (PrAppKey k = PrAppKey_Left; k <= PrAppKey_GoBack; ++k) {
        auto label = prAppKeyName(k);
        if (label.data && name == std::string_view(label.data, label.size)) {
            key = k;
            return true;
        }
    }
    return false;
}

bool shortcut(const PrJson* d, Index row, Shortcut& out) {
    if (prJsonKind(d, row) != PrJsonKind_Array ||
            !chord(stringAt(d, prJsonElement(d, row, 0)), out.key, out.modifiers))
        return false;
    const auto op = stringAt(d, prJsonElement(d, row, 1));
    if (op == "select_tab" || op == "zoom") {
        const auto arg = prJsonElement(d, row, 2);
        const double value = prJsonNumber(d, arg, 0);
        if (prJsonCount(d, row) != 3 || prJsonKind(d, arg) != PrJsonKind_Number || value != std::trunc(value) ||
                value < (op == "zoom" ? -1024 : 0) || value > 1024)
            return false;
        out.command = op == "zoom" ? WindowCommand::Zoom : WindowCommand::SelectTab;
        out.argument = static_cast<int>(value);
        return true;
    }
    if (op == "open_tab") {
        out.command = WindowCommand::OpenTab;
        out.type = stringAt(d, prJsonElement(d, row, 2));
        return prJsonCount(d, row) == 3 && !out.type.empty();
    }
    if (prJsonCount(d, row) != 2) return false;
    if (op == "open_terminal") out.command = WindowCommand::OpenTerminal;
    else if (op == "close_terminal") out.command = WindowCommand::CloseTerminal;
    else if (op == "close_tab") out.command = WindowCommand::CloseTab;
    else if (op == "invoke_service") out.command = WindowCommand::InvokeService;
    else if (op == "stop") out.command = WindowCommand::Stop;
    else return false;
    return true;
}

bool readWindow(const std::string& answer, std::string_view tab, bool promptOpen, std::vector<Shortcut>& keys,
    std::vector<Panel>& panels, std::map<std::string, TabType>& types, std::string& label, std::string& why) {
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> doc(prJsonCreate(), prJsonDestroy);
    auto* d = doc.get();
    if (!prJsonParse(d, {answer.data(), answer.size()}, nullptr))
        return false;
    const auto rows = prJsonMember(d, 0, {"shortcuts", 9});
    const auto list = prJsonMember(d, 0, {"panels", 6});
    if (prJsonKind(d, rows) != PrJsonKind_Array || prJsonKind(d, list) != PrJsonKind_Array ||
            prJsonCount(d, rows) > 128 || prJsonCount(d, list) > 5) {
        why = "return arrays of up to 128 shortcuts and 5 panels";
        return false;
    }
    const auto records = prJsonMember(d, 0, {"tab_types", 9});
    if (records != kPrJsonMissing && (prJsonKind(d, records) != PrJsonKind_Array || prJsonCount(d, records) > 128)) {
        why = "tab_types must be a list of at most 128 registrations";
        return false;
    }
    for (uint32_t i = 0; i < prJsonCount(d, records); ++i) {
        const auto row = prJsonElement(d, records, i);
        auto field = [&](std::string_view name) {
            return std::string(stringAt(d, prJsonMember(d, row, {name.data(), name.size()})));
        };
        const auto id = field("id");
        TabType spec{field("title"), field("group"), field("native"), field("service")};
        if (id.empty() || id.size() > 64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != id.npos ||
                spec.group.empty() || spec.group.size() > 64 || spec.title.empty() || spec.title.size() > 128 ||
                !types.emplace(id, std::move(spec)).second) {
            why = "tab types need unique identifiers, a title and a group";
            return false;
        }
    }
    for (uint32_t i = 0; i < prJsonCount(d, rows); ++i) {
        Shortcut binding{};
        if (!shortcut(d, prJsonElement(d, rows, i), binding) || std::any_of(keys.begin(), keys.end(),
                [&](const Shortcut& k) { return k.key == binding.key && k.modifiers == binding.modifiers; })) {
            why = "invalid or repeated shortcut at index " + std::to_string(i);
            return false;
        }
        if (binding.command == WindowCommand::OpenTab && !types.count(binding.type)) {
            why = "open_tab refers to an unregistered type " + binding.type;
            return false;
        }
        keys.push_back(binding);
    }
    for (uint32_t i = 0; i < prJsonCount(d, list); ++i) {
        const auto name = stringAt(d, prJsonElement(d, list, i));
        Panel panel;
        if (name == "board" && tab == "sprint") panel = Panel::Board;
        else if (name == "terminal" && tab == "terminal") panel = Panel::Terminal;
        else if (name == "transcript" && (tab == "friend" || tab == "empty")) panel = Panel::Transcript;
        else if (name == "prompt" && tab == "friend" && promptOpen) panel = Panel::Prompt;
        else if (name == "status") panel = Panel::Status;
        else if (name == "service_reply" && tab == "extension") panel = Panel::ServiceReply;
        else if (name == "native" && tab == "extension") panel = Panel::Native;
        else if (name == "text_area" && tab == "extension") panel = Panel::TextArea;
        else if (name == "label" && tab == "extension") {
            label = stringAt(d, prJsonMember(d, 0, {"label", 5}));
            panel = Panel::Label;
        }
        else { why = "unavailable panel at index " + std::to_string(i); return false; }
        if (std::find(panels.begin(), panels.end(), panel) != panels.end()) {
            why = "each panel can appear once";
            return false;
        }
        panels.push_back(panel);
    }
    return true;
}
} // namespace

void WindowApplication::update(policy::Host& host, const char* tab, const std::string& type, bool promptOpen,
    bool hasStatus) {
    const auto context = std::string("{\"tab\":\"") + tab + "\",\"type\":\"" + type + "\",\"prompt_open\":" + (promptOpen ? "true" : "false") +
        ",\"has_status\":" + (hasStatus ? "true}" : "false}");
    const bool sameContext = context == lastContext_;
    const auto generation = host.refresh();
    if (sameContext && generation == generation_) return;
    lastContext_ = context;
    generation_ = generation;
    std::string why;
    std::vector<Shortcut> keys;
    std::vector<Panel> parts;
    std::map<std::string, TabType> registrations;
    std::string caption;
    const auto answer = host.decide({}, "app.window", context, why);
    if (why.empty() && readWindow(answer, tab, promptOpen, keys, parts, registrations, caption, why)) {
        shortcuts = std::move(keys);
        panels = std::move(parts);
        types = std::move(registrations);
        label = std::move(caption);
    } else {
        std::fprintf(stderr, "nexus: window application: %s. Check client/window.star and apply again.\n", why.c_str());
        if (sameContext)
            return;
        keys.clear();
        parts.clear();
        registrations.clear();
        caption.clear();
        policy::Host stock("client");
        const auto fallback = stock.decide({}, "app.window", context, why);
        if (!readWindow(fallback, tab, promptOpen, keys, parts, registrations, caption, why)) {
            keys.clear();
            parts.clear();
        }
        shortcuts = std::move(keys);
        panels = std::move(parts);
        types = std::move(registrations);
        label = std::move(caption);
    }
}
} // namespace nexus
