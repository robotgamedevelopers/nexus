#include "window_app.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <memory>
#include <set>
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
    if (op == "select_tab" || op == "move_tab" || op == "zoom") {
        const auto arg = prJsonElement(d, row, 2);
        const double value = prJsonNumber(d, arg, 0);
        if (prJsonCount(d, row) != 3 || prJsonKind(d, arg) != PrJsonKind_Number || value != std::trunc(value) ||
                value < (op == "select_tab" ? 0 : -1024) || value > 1024 ||
                (op == "move_tab" && value != -1 && value != 1))
            return false;
        out.command = op == "zoom" ? WindowCommand::Zoom :
            op == "move_tab" ? WindowCommand::MoveTab : WindowCommand::SelectTab;
        out.argument = static_cast<int>(value);
        return true;
    }
    if (op == "account_picker") {
        out.command = WindowCommand::AccountPicker;
        out.type = stringAt(d, prJsonElement(d, row, 2));
        return prJsonCount(d, row) == 3 && (out.type == "open" || out.type == "previous" ||
            out.type == "next" || out.type == "confirm" || out.type == "cancel");
    }
    if (op == "open_tab" || op == "account_command") {
        out.command = op == "open_tab" ? WindowCommand::OpenTab : WindowCommand::AccountCommand;
        out.type = stringAt(d, prJsonElement(d, row, 2));
        return prJsonCount(d, row) == 3 && !out.type.empty() &&
            (op != "account_command" || (out.type.size() <= 63 && out.type.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos));
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

bool groupName(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::none_of(name.begin(), name.end(),
        [](unsigned char c) { return c < 32 || c == 127; });
}

bool readGroups(const PrJson* d, const std::map<std::string, TabType>& types, std::vector<TabGroup>& groups,
        std::optional<bool>& closable, std::string& why) {
    const auto rows = prJsonMember(d, 0, {"tab_groups", 10});
    if (rows == kPrJsonMissing) return true; // original window applications used close_terminal and close_tab
    const auto mayClose = prJsonKind(d, prJsonMember(d, 0, {"closable", 8}));
    if (prJsonKind(d, rows) != PrJsonKind_Array || prJsonCount(d, rows) > 256 ||
            (mayClose != PrJsonKind_False && mayClose != PrJsonKind_True)) {
        why = "return up to 256 tab_groups and a boolean closable for the active tab";
        return false;
    }
    closable = mayClose == PrJsonKind_True;
    std::set<std::string> members{"friend", "terminal", "sprint"}, ids;
    for (const auto& type : types) {
        if (!members.insert(type.first).second) {
            why = "tab type conflicts with a native type: " + type.first;
            return false;
        }
    }
    for (uint32_t i = 0; i < prJsonCount(d, rows); ++i) {
        const auto row = prJsonElement(d, rows, i);
        TabGroup group;
        group.id = stringAt(d, prJsonMember(d, row, {"id", 2}));
        const auto flag = prJsonKind(d, prJsonMember(d, row, {"closable", 8}));
        if (!groupName(group.id) || !ids.insert(group.id).second ||
                (flag != PrJsonKind_True && flag != PrJsonKind_False)) {
            why = "tab groups need a unique id and a boolean closable";
            return false;
        }
        group.closable = flag == PrJsonKind_True;
        const auto list = prJsonMember(d, row, {"types", 5});
        const auto from = prJsonMember(d, row, {"accepts_from", 12});
        if (prJsonKind(d, list) != PrJsonKind_Array || (from != kPrJsonMissing &&
                (prJsonKind(d, from) != PrJsonKind_Array || prJsonCount(d, from) > 256))) {
            why = "group types and accepts_from must be lists";
            return false;
        }
        for (uint32_t j = 0; j < prJsonCount(d, list); ++j) {
            std::string type(stringAt(d, prJsonElement(d, list, j)));
            if (!members.erase(type)) {
                why = "unknown or repeated tab group member: " + type;
                return false;
            }
            group.types.push_back(std::move(type));
        }
        for (uint32_t j = 0; j < prJsonCount(d, from); ++j)
            group.acceptsFrom.emplace_back(stringAt(d, prJsonElement(d, from, j)));
        groups.push_back(std::move(group));
    }
    if (!members.empty()) {
        why = "tab_groups must assign every type, including " + *members.begin();
        return false;
    }
    for (const auto& group : groups)
        for (const auto& source : group.acceptsFrom)
            if (!ids.count(source)) {
                why = "accepts_from names an unknown group: " + source;
                return false;
            }
    return true;
}

bool readColor(const PrJson* d, Index obj, PrStringView name, unsigned& value) {
    const auto field = prJsonMember(d, obj, name);
    if (field == kPrJsonMissing) return true;
    const auto text = stringAt(d, field);
    if (text.size() != 7 || text.front() != '#') return false;
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data() + 1, end, value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool readPoints(const PrJson* d, Index obj, PrStringView name, float& value, double min, double max) {
    const auto field = prJsonMember(d, obj, name);
    if (field == kPrJsonMissing) return true;
    const double n = prJsonNumber(d, field, -1);
    if (prJsonKind(d, field) != PrJsonKind_Number || !(min <= n && n <= max)) return false;
    value = static_cast<float>(n);
    return true;
}

bool readFlag(const PrJson* d, Index obj, PrStringView name, bool& value) {
    const auto field = prJsonMember(d, obj, name);
    if (field == kPrJsonMissing) return true;
    const auto kind = prJsonKind(d, field);
    if (kind != PrJsonKind_True && kind != PrJsonKind_False) return false;
    value = kind == PrJsonKind_True;
    return true;
}

bool readMessageCard(const PrJson* d, Index obj, MessageCardStyle& card) {
    card = {};
    if (obj == kPrJsonMissing) return true;
    return prJsonKind(d, obj) == PrJsonKind_Object &&
        readColor(d, obj, {"fill", 4}, card.fill) && readColor(d, obj, {"ink", 3}, card.ink) &&
        readColor(d, obj, {"email_ink", 9}, card.emailInk) &&
        readPoints(d, obj, {"text_size", 9}, card.textSize, 12, 72) &&
        readPoints(d, obj, {"radius", 6}, card.radius, 0, 64);
}

bool readTranscriptStyle(const PrJson* d, Index obj, TranscriptStyle& style, std::string& why) {
    style = {};
    if (obj == kPrJsonMissing) return true;
    if (prJsonKind(d, obj) != PrJsonKind_Object) {
        why = "transcript_style must be an object";
        return false;
    }
    for (auto i = prJsonFirst(d, obj); i != kPrJsonMissing; i = prJsonAfter(d, obj, i)) {
        const auto key = stringAt(d, i);
        if (key != "fill" && key != "ink" && key != "person_ink" && key != "notice_ink" && key != "status_ink" && key != "switch_email_ink" &&
                key != "radius" && key != "show_notices" && key != "show_tools") {
            why = "unknown transcript_style field: " + std::string(key);
            return false;
        }
    }
    const bool valid =
        readColor(d, obj, {"fill", 4}, style.fill) && readColor(d, obj, {"ink", 3}, style.ink) &&
        readColor(d, obj, {"person_ink", 10}, style.personInk) &&
        readColor(d, obj, {"switch_email_ink", 16}, style.switchEmailInk) &&
        readColor(d, obj, {"notice_ink", 10}, style.noticeInk) && readColor(d, obj, {"status_ink", 10}, style.statusInk) &&
        readPoints(d, obj, {"radius", 6}, style.radius, 0, 64) &&
        readFlag(d, obj, {"show_notices", 12}, style.showNotices) && readFlag(d, obj, {"show_tools", 10}, style.showTools);
    if (prJsonMember(d, obj, {"person_ink", 10}) == kPrJsonMissing)
        style.personInk = style.ink;
    if (!valid)
        why = "transcript_style needs #RRGGBB colors (fill, ink, person_ink, notice_ink, status_ink, switch_email_ink), "
              "radius from 0 to 64 and boolean show_notices and show_tools";
    return valid;
}

bool readWindow(const std::string& answer, std::string_view tab, bool promptOpen, MessageCardStyle& card,
    TranscriptStyle& transcript, std::vector<Shortcut>& keys, std::vector<Panel>& panels, std::map<std::string, TabType>& types,
    std::vector<TabGroup>& groups, std::optional<bool>& closable, std::string& label, std::string& autoLabel, float& overlayOpacity, std::string& why) {
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> doc(prJsonCreate(), prJsonDestroy);
    auto* d = doc.get();
    if (!prJsonParse(d, {answer.data(), answer.size()}, nullptr))
        return false;
    const auto name = prJsonMember(d, 0, {"account_auto_label", 18});
    autoLabel = name == kPrJsonMissing ? "auto" : stringAt(d, name);
    if (!groupName(autoLabel)) {
        why = "account_auto_label must be a nonempty display name of up to 64 bytes";
        return false;
    }
    const auto opacity = prJsonMember(d, 0, {"overlay_opacity", 15});
    if (!readMessageCard(d, prJsonMember(d, 0, {"message_card", 12}), card)) {
        why = "message_card needs #RRGGBB fill, ink and email_ink, text_size from 12 to 72, and radius from 0 to 64";
        return false;
    }
    if (!readTranscriptStyle(d, prJsonMember(d, 0, {"transcript_style", 16}), transcript, why))
        return false;
    overlayOpacity = style::kOverlayOpacity;
    if (opacity != kPrJsonMissing) {
        const double alpha = prJsonNumber(d, opacity, -1);
        if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) {
            why = "overlay_opacity must be a number from 0 to 1";
            return false;
        }
        overlayOpacity = static_cast<float>(alpha);
    }
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
        const auto mayClose = prJsonMember(d, row, {"closable", 8});
        if (mayClose != kPrJsonMissing && prJsonKind(d, mayClose) != PrJsonKind_True &&
                prJsonKind(d, mayClose) != PrJsonKind_False) {
            why = "tab type closable must be a boolean";
            return false;
        }
        if (id.empty() || id.size() > 64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != id.npos ||
                spec.group.empty() || spec.group.size() > 64 || spec.title.empty() || spec.title.size() > 128 ||
                !types.emplace(id, std::move(spec)).second) {
            why = "tab types need unique identifiers, a title and a group";
            return false;
        }
    }
    if (!readGroups(d, types, groups, closable, why)) return false;
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
        else if (name == "account" && tab == "friend") panel = Panel::Account;
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
    bool hasStatus, std::size_t activeTab, std::size_t tabCount, const std::string& group, SignInAccount account, bool pickerOpen) {
    std::string escapedGroup;
    for (const char c : group) {
        if (c == '\\' || c == '"') escapedGroup += '\\';
        escapedGroup += c;
    }
    auto context = std::string("{\"tab\":\"") + tab + "\",\"type\":\"" + type + "\",\"prompt_open\":" + (promptOpen ? "true" : "false") +
        ",\"has_status\":" + (hasStatus ? "true" : "false") + ",\"active_tab\":" + std::to_string(activeTab) +
        ",\"account_picker_open\":" + (pickerOpen ? "true" : "false") +
        ",\"needs_sign_in\":" + (account.name.empty() ? "false" : "true") + ",\"tab_count\":" + std::to_string(tabCount) + ",\"group\":\"" + escapedGroup + "\"}";
    const bool sameContext = context == lastContext_ && account == signInAccount;
    const auto generation = host.refresh();
    if (sameContext && generation == generation_) return;
    lastContext_ = context;
    generation_ = generation;
    signInAccount = std::move(account);
    context.pop_back();
    context += ",\"sign_in_account\":";
    if (signInAccount.name.empty()) {
        context += "null}";
    } else {
        std::unique_ptr<PrJsonWriter, decltype(&prJsonWriterDestroy)> json(prJsonWriterCreate(), prJsonWriterDestroy);
        auto field = [&](PrStringView key, const std::string& value) {
            prJsonWriterKey(json.get(), key);
            prJsonWriterString(json.get(), {value.data(), value.size()});
        };
        prJsonWriterBeginObject(json.get());
        field({"name", 4}, signInAccount.name);
        field({"label", 5}, signInAccount.label);
        field({"email", 5}, signInAccount.email);
        prJsonWriterEndObject(json.get());
        const auto text = prJsonWriterText(json.get());
        context.append(text.data, text.size);
        context += '}';
    }
    std::string why;
    MessageCardStyle card;
    TranscriptStyle transcript;
    std::vector<Shortcut> keys;
    std::vector<Panel> parts;
    std::map<std::string, TabType> registrations;
    std::vector<TabGroup> layout;
    std::optional<bool> mayClose;
    std::string caption, autoLabel;
    float opacity = style::kOverlayOpacity;
    const auto answer = host.decide({}, "app.window", context, why);
    if (why.empty() && readWindow(answer, tab, promptOpen, card, transcript, keys, parts, registrations, layout, mayClose, caption, autoLabel, opacity, why)) {
        if (generation != appliedGeneration_ || groups != layout || types != registrations)
            ++layoutGeneration;
        appliedGeneration_ = generation;
        shortcuts = std::move(keys);
        panels = std::move(parts);
        types = std::move(registrations);
        groups = std::move(layout);
        closable = mayClose;
        label = std::move(caption);
        autoAccountLabel = std::move(autoLabel);
        overlayOpacity = opacity;
        messageCard = card;
        transcriptStyle = transcript;
    } else {
        std::fprintf(stderr, "nexus: window application: %s. Check client/window.star and apply again.\n", why.c_str());
        if (sameContext)
            return;
        keys.clear();
        parts.clear();
        registrations.clear();
        layout.clear();
        mayClose.reset();
        caption.clear();
        policy::Host stock("client");
        const auto fallback = stock.decide({}, "app.window", context, why);
        if (!readWindow(fallback, tab, promptOpen, card, transcript, keys, parts, registrations, layout, mayClose, caption, autoLabel, opacity, why)) {
            keys.clear();
            parts.clear();
            registrations.clear();
            layout.clear();
            mayClose.reset();
        }
        shortcuts = std::move(keys);
        panels = std::move(parts);
        if (appliedGeneration_) {
            // Keep the last accepted layout across a rejected edit. Its previous
            // active-tab permission cannot authorize closing a different tab.
            closable = false;
        } else {
            types = std::move(registrations);
            groups = std::move(layout);
            closable = mayClose;
        }
        label = std::move(caption);
        autoAccountLabel = std::move(autoLabel);
        overlayOpacity = opacity;
        messageCard = card;
        transcriptStyle = transcript;
    }
}
} // namespace nexus
