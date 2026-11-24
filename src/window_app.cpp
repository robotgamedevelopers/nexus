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

bool accountAction(std::string_view action) {
    if (action == "picker:model" || action == "picker:effort" || action == "picker:close") return true;
    if (action.starts_with("point:") || action.starts_with("choose:")) {
        const auto value = action.substr(action.find(':') + 1);
        return !value.empty() && value.size() <= 128 &&
            value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-") == value.npos;
    }
    if (action == "dismiss" || action == "reveal") return true;
    const auto colon = action.find(':');
    if (colon == action.npos || colon + 1 == action.size() || action.size() > 80) return false;
    const auto op = action.substr(0, colon);
    return op == "focus" || op == "select" || op == "sign_in";
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
    if (op == "scroll_transcript") {
        out.command = WindowCommand::ScrollTranscript;
        out.type = stringAt(d, prJsonElement(d, row, 2));
        return prJsonCount(d, row) == 3 && (out.type == "top" || out.type == "bottom" ||
               out.type == "page_up" || out.type == "page_down");
    }
    if (op == "replace_draft") {
        out.command = WindowCommand::ReplaceDraft;
        out.type = stringAt(d, prJsonElement(d, row, 2));
        return prJsonCount(d, row) == 3 && prJsonKind(d, prJsonElement(d, row, 2)) == PrJsonKind_String && out.type.size() <= 32768;
    }
    if (op == "resolve_limit") {
        out.command = WindowCommand::ResolveLimit;
        out.type = stringAt(d, prJsonElement(d, row, 2));
        return prJsonCount(d, row) == 3 && (out.type == "account" || out.type == "model");
    }
    if (op == "account_action") {
        out.command = WindowCommand::AccountAction;
        out.type = stringAt(d, prJsonElement(d, row, 2));
        return prJsonCount(d, row) == 3 && accountAction(out.type);
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
    else if (op == "clear_composer") out.command = WindowCommand::ClearComposer;
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

bool readOptionalColor(const PrJson* d, Index obj, PrStringView name, std::optional<unsigned>& value) {
    if (prJsonMember(d, obj, name) == kPrJsonMissing) return true;
    unsigned color = 0;
    if (!readColor(d, obj, name, color)) return false;
    value = color;
    return true;
}

bool readPoints(const PrJson* d, Index obj, PrStringView name, float& value, double min, double max) {
    const auto field = prJsonMember(d, obj, name);
    if (field == kPrJsonMissing) return true;
    const double n = prJsonNumber(d, field, -1);
    if (prJsonKind(d, field) != PrJsonKind_Number || !(min <= n && n <= max)) return false;
    value = static_cast<float>(n);
    return true;
}

// Policy supplies normalized time/value pairs, evaluated once when the application changes.
bool readCurve(const PrJson* doc, Index object, PrStringView key, MotionCurve& curve) {
    const auto array = prJsonMember(doc, object, key);
    if (array == kPrJsonMissing) return true;
    const auto count = prJsonCount(doc, array);
    if (prJsonKind(doc, array) != PrJsonKind_Array || count < 2 || count > 64) return false;
    curve.clear();
    for (unsigned i = 0; i < count; ++i) {
        const auto pair = prJsonElement(doc, array, i);
        if (prJsonKind(doc, pair) != PrJsonKind_Array || prJsonCount(doc, pair) != 2) return false;
        const auto time = prJsonNumber(doc, prJsonElement(doc, pair, 0), -1);
        const auto value = prJsonNumber(doc, prJsonElement(doc, pair, 1), -1);
        if (!(time >= 0 && time <= 1 && value >= 0 && value <= 1) ||
                (!curve.empty() && time <= curve.back().first)) return false;
        curve.emplace_back(time, value);
    }
    return curve.front().first == 0 && curve.back().first == 1;
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
        readPoints(d, obj, {"selector_width_em", 17}, card.selectorWidthEm, 6, 80) &&
        readPoints(d, obj, {"vertical_extension", 18}, card.verticalExtension, 0, 50) &&
        readPoints(d, obj, {"side_extension", 14}, card.sideExtension, 0, 50) &&
        readPoints(d, obj, {"row_height", 10}, card.rowHeight, 1, 4) &&
        readPoints(d, obj, {"row_gap", 7}, card.rowGap, 0, 4) &&
        readPoints(d, obj, {"heading_gap", 11}, card.headingGap, 0, 4) &&
        readPoints(d, obj, {"unselected_weight", 17}, card.unselectedWeight, 100, 900) &&
        readPoints(d, obj, {"selected_radius", 15}, card.selectedRadius, 0, 64) &&
        readPoints(d, obj, {"email_weight", 12}, card.emailWeight, 100, 900) &&
        readPoints(d, obj, {"selected_opacity", 16}, card.selectedOpacity, 0, 1) &&
        readColor(d, obj, {"selected_fill", 13}, card.selectedFill) &&
        readColor(d, obj, {"selected_ink", 12}, card.selectedInk) &&
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
        if (key != "fill" && key != "ink" && key != "person_ink" && key != "bus_ink" && key != "message_inks" && key != "notice_ink" && key != "status_ink" && key != "switch_email_ink" &&
                key != "radius" && key != "show_notices" && key != "show_tools" && key != "reply_width" && key != "markdown" && key != "conversation_panel" && key != "tool_prompts" && key != "list_gap_em" &&
                key != "panel_fill" && key != "link_ink" && key != "pending_text_pulse" && key != "pending_text_seconds" && key != "pending_dash_seconds" && key != "pending_outline_width" && key != "sent_outline_width" && key != "sent_outline_ink" && key != "read_path_ink" && key != "command_ink" && key != "code_ink" && key != "code_fill" &&
                key != "code_background" && key != "code_padding" && key != "code_radius" && key != "code_size_offset" &&
                key != "glyph_peak" && key != "glyph_seconds" && key != "glyph_stagger" &&
                key != "person_bold" && key != "person_flash_peak" && key != "person_flash_seconds" &&
                key != "scanner_curve" && key != "scanner_pause" && key != "scanner_ease" && key != "scanner_fade" &&
                key != "scanner_gap" && key != "scanner_offset" && key != "highlight_seconds" && key != "flash_seconds" &&
                key != "scanner_ink" && key != "scanner_seconds" && key != "scanner_peak" && key != "scanner_pulse_floor" && key != "highlight_energy") {
            why = "unknown transcript_style field: " + std::string(key);
            return false;
        }
    }
    const auto messageInks = prJsonMember(d, obj, {"message_inks", 12});
    if (messageInks != kPrJsonMissing) {
        if (prJsonKind(d, messageInks) != PrJsonKind_Object) return false;
        for (auto i = prJsonFirst(d, messageInks); i != kPrJsonMissing; i = prJsonAfter(d, messageInks, i)) {
            const auto text = stringAt(d, i);
            unsigned color{};
            if (!readColor(d, messageInks, {text.data(), text.size()}, color)) return false;
            style.messageInks.emplace(text, color);
        }
    }
    const auto prompts = prJsonMember(d, obj, {"tool_prompts", 12});
    if (prompts != kPrJsonMissing) {
        if (prJsonKind(d, prompts) != PrJsonKind_Object) {
            why = "tool_prompts must map tool names to prompt strings";
            return false;
        }
        for (auto i = prJsonFirst(d, prompts); i != kPrJsonMissing; i = prJsonAfter(d, prompts, i)) {
            const auto name = stringAt(d, i);
            const auto value = prJsonMember(d, prompts, {name.data(), name.size()});
            if (prJsonKind(d, value) != PrJsonKind_String) {
                why = "tool_prompts needs a string for " + std::string(name);
                return false;
            }
            style.toolPrompts.emplace(name, stringAt(d, value));
        }
    }
    const bool valid =
        readColor(d, obj, {"fill", 4}, style.fill) && readColor(d, obj, {"ink", 3}, style.ink) &&
        readPoints(d, obj, {"glyph_stagger", 13}, style.glyphStagger, 0, 0.3) &&
        readPoints(d, obj, {"glyph_peak", 10}, style.glyphPeak, 1, 8) &&
        readPoints(d, obj, {"glyph_seconds", 13}, style.glyphSeconds, 0.05, 3) &&
        readFlag(d, obj, {"person_bold", 11}, style.personBold) &&
        readPoints(d, obj, {"person_flash_peak", 17}, style.personFlashPeak, 1, 8) &&
        readPoints(d, obj, {"person_flash_seconds", 20}, style.personFlashSeconds, 0.05, 3) &&
        readCurve(d, obj, {"scanner_curve", 13}, style.scannerCurve) &&
        readPoints(d, obj, {"scanner_pause", 13}, style.scannerPause, 0, 3) &&
        readPoints(d, obj, {"scanner_ease", 12}, style.scannerEase, 0.005, 2) &&
        readPoints(d, obj, {"scanner_fade", 12}, style.scannerFade, 0.005, 3) &&
        readPoints(d, obj, {"scanner_gap", 11}, style.scannerGap, 0, 128) &&
        readPoints(d, obj, {"scanner_offset", 14}, style.scannerOffset, -128, 128) &&
        readPoints(d, obj, {"highlight_seconds", 17}, style.highlightSeconds, 0.4, 10) &&
        readPoints(d, obj, {"flash_seconds", 13}, style.flashSeconds, 0.04, 2) &&
        readPoints(d, obj, {"highlight_energy", 16}, style.highlightEnergy, 0, 4) &&
        readOptionalColor(d, obj, {"scanner_ink", 11}, style.scannerInk) &&
        readPoints(d, obj, {"scanner_seconds", 15}, style.scannerSeconds, 0.4, 10) &&
        readPoints(d, obj, {"scanner_pulse_floor", 19}, style.scannerPulseFloor, 0, 1) &&
        readPoints(d, obj, {"scanner_peak", 12}, style.scannerPeak, 1, 8) &&
        readColor(d, obj, {"person_ink", 10}, style.personInk) &&
        readColor(d, obj, {"bus_ink", 7}, style.busInk) &&
        readOptionalColor(d, obj, {"read_path_ink", 13}, style.readPathInk) &&
        readOptionalColor(d, obj, {"command_ink", 11}, style.commandInk) &&
        readOptionalColor(d, obj, {"code_ink", 8}, style.codeInk) &&
        readColor(d, obj, {"code_fill", 9}, style.codeFill) &&
        readFlag(d, obj, {"code_background", 15}, style.codeBackground) &&
        readPoints(d, obj, {"code_size_offset", 16}, style.codeSizeOffset, -8, 32) &&
        readPoints(d, obj, {"code_padding", 12}, style.codePadding, 0, 32) &&
        readPoints(d, obj, {"code_radius", 11}, style.codeRadius, 0, 64) &&
        readOptionalColor(d, obj, {"panel_fill", 10}, style.panelFill) &&
        readOptionalColor(d, obj, {"link_ink", 8}, style.linkInk) &&
        readColor(d, obj, {"switch_email_ink", 16}, style.switchEmailInk) &&
        readColor(d, obj, {"notice_ink", 10}, style.noticeInk) && readColor(d, obj, {"status_ink", 10}, style.statusInk) &&
        readColor(d, obj, {"sent_outline_ink", 16}, style.sentOutlineInk) &&
        readPoints(d, obj, {"sent_outline_width", 18}, style.sentOutlineWidth, 0, 8) &&
        readPoints(d, obj, {"pending_text_pulse", 18}, style.pendingTextPulse, 0, 0.5f) &&
        readPoints(d, obj, {"pending_text_seconds", 20}, style.pendingTextSeconds, 0.2f, 30) &&
        readPoints(d, obj, {"pending_dash_seconds", 20}, style.pendingDashSeconds, 0, 60) &&
        readPoints(d, obj, {"pending_outline_width", 21}, style.pendingOutlineWidth, 0, 8) &&
        readPoints(d, obj, {"radius", 6}, style.radius, 0, 64) &&
        readFlag(d, obj, {"show_notices", 12}, style.showNotices) && readFlag(d, obj, {"show_tools", 10}, style.showTools) &&
        readPoints(d, obj, {"list_gap_em", 11}, style.listGap, 0, 4) &&
        readPoints(d, obj, {"reply_width", 11}, style.replyWidth, 1, 100) && readFlag(d, obj, {"markdown", 8}, style.markdown) && readFlag(d, obj, {"conversation_panel", 18}, style.conversationPanel);
    if (prJsonMember(d, obj, {"person_ink", 10}) == kPrJsonMissing)
        style.personInk = style.ink;
    if (!valid)
        why = "transcript_style needs #RRGGBB colors (fill, ink, person_ink, notice_ink, status_ink, switch_email_ink), "
              "radius from 0 to 64 and boolean show_notices and show_tools";
    return valid;
}

bool readSurface(const PrJson* doc, AccountSurface& surface) {
    surface = {};
    const auto sounds = prJsonMember(doc, 0, {"sounds", 6});
    if (sounds != kPrJsonMissing) {
        auto cue = [&](PrStringView name, Chirp& chirp) {
            const auto item = prJsonMember(doc, sounds, name);
            return item == kPrJsonMissing || (prJsonKind(doc, item) == PrJsonKind_Object &&
                readPoints(doc, item, {"from_hz", 7}, chirp.startHz, 40, 8000) &&
                readPoints(doc, item, {"to_hz", 5}, chirp.endHz, 40, 8000) &&
                readPoints(doc, item, {"duration_ms", 11}, chirp.durationMs, 5, 1500) &&
                readFlag(doc, item, {"synth", 5}, chirp.synth) &&
                readPoints(doc, item, {"gain", 4}, chirp.gain, 0, 1));
        };
        if (prJsonKind(doc, sounds) != PrJsonKind_Object || !cue({"copy", 4}, surface.copyChirp) || !cue({"tab", 3}, surface.tabChirp) ||
                !cue({"confirm", 7}, surface.confirmChirp) ||
                !cue({"open", 4}, surface.openChirp) || !cue({"close", 5}, surface.closeChirp) ||
                !cue({"dialog", 6}, surface.dialogChirp) || !cue({"cancel", 6}, surface.cancelChirp) ||
                !cue({"startup", 7}, surface.startupChirp) || !cue({"step", 4}, surface.stepChirp) || !cue({"discard", 7}, surface.discardChirp) || !cue({"hold", 4}, surface.holdChirp) || !cue({"send", 4}, surface.sendChirp) ||
                !cue({"ready", 5}, surface.readyChirp)) return false;
    }
    auto& tabs = surface.tabs;
    const auto style = prJsonMember(doc, 0, {"tab_style", 9});
    if (style != kPrJsonMissing && (prJsonKind(doc, style) != PrJsonKind_Object ||
            !readColor(doc, style, {"fill", 4}, tabs.fill) || !readColor(doc, style, {"ink", 3}, tabs.ink) ||
            !readColor(doc, style, {"divider", 7}, tabs.divider) ||
            !readColor(doc, style, {"selected_fill", 13}, tabs.selectedFill) ||
            !readColor(doc, style, {"selected_ink", 12}, tabs.selectedInk) ||
            !readPoints(doc, style, {"opacity", 7}, tabs.opacity, 0, 1) ||
            !readPoints(doc, style, {"selected_opacity", 16}, tabs.selectedOpacity, 0, 1) ||
            !readPoints(doc, style, {"selected_radius", 15}, tabs.selectedRadius, 0, 64) ||
            !readPoints(doc, style, {"weight", 6}, tabs.weight, 100, 900) ||
            !readPoints(doc, style, {"selected_weight", 15}, tabs.selectedWeight, 100, 900) ||
            !readPoints(doc, style, {"height", 6}, tabs.height, 12, 256) ||
            !readPoints(doc, style, {"tab_height", 10}, tabs.tabHeight, 10, 256) ||
            !readPoints(doc, style, {"gap", 3}, tabs.gap, 0, 128) ||
            !readPoints(doc, style, {"leading", 7}, tabs.leading, 0, 128) ||
            !readFlag(doc, style, {"reserve_width", 13}, tabs.reserveWidth))) return false;
    auto& prompt = surface.prompt;
    const auto box = prJsonMember(doc, 0, {"prompt_style", 12});
    if (box != kPrJsonMissing && (prJsonKind(doc, box) != PrJsonKind_Object ||
            !readColor(doc, box, {"fill", 4}, prompt.fill) || !readColor(doc, box, {"border", 6}, prompt.border) ||
            !readColor(doc, box, {"ink", 3}, prompt.ink) || !readColor(doc, box, {"selection", 9}, prompt.selection) ||
            !readPoints(doc, box, {"width", 5}, prompt.width, 10, 100) ||
            !readPoints(doc, box, {"radius", 6}, prompt.radius, 0, 128) ||
            !readPoints(doc, box, {"padding_x", 9}, prompt.paddingX, 0, 128) ||
            !readPoints(doc, box, {"height", 6}, prompt.height, 1, 256) ||
            !readPoints(doc, box, {"gap", 3}, prompt.gap, 0, 128) ||
            !readPoints(doc, box, {"max_lines", 9}, prompt.maxLines, 1, 64) ||
            !readColor(doc, box, {"caret_ink", 9}, prompt.caretInk) ||
            !readPoints(doc, box, {"caret_offset_y", 14}, prompt.caretOffsetY, -16, 16) ||
            !readPoints(doc, box, {"caret_gap", 9}, prompt.caretGap, 0, 32) ||
            !readPoints(doc, box, {"caret_height", 12}, prompt.caretHeight, 1, style::kBodySize) ||
            !readPoints(doc, box, {"caret_radius", 12}, prompt.caretRadius, 0, 32) ||
            !readCurve(doc, box, {"caret_curve", 11}, prompt.caretCurve) ||
            !readPoints(doc, box, {"caret_hold", 10}, prompt.caretHold, 0, 0.35) ||
            !readPoints(doc, box, {"caret_fall_ratio", 16}, prompt.caretFallRatio, 0.1, 4) ||
            !readPoints(doc, box, {"caret_seconds", 13}, prompt.caretSeconds, 0.4, 10) ||
            !readPoints(doc, box, {"caret_aspect", 12}, prompt.caretAspect, 0.1f, 4) ||
            !readPoints(doc, box, {"text_peak", 9}, prompt.textPeak, 1, 8) ||
            !readPoints(doc, box, {"caret_peak", 10}, prompt.caretPeak, 1, 8) ||
            !readFlag(doc, box, {"bold", 4}, prompt.bold) ||
            !readFlag(doc, box, {"show_plus", 9}, prompt.showPlus))) return false;
    if (!readFlag(doc, 0, {"prompt_command", 14}, surface.promptCommand) ||
            !readFlag(doc, 0, {"show_tabs", 9}, surface.showTabs) ||
            !readFlag(doc, 0, {"reorder_tabs", 12}, surface.reorderTabs)) return false;
    const auto titles = prJsonMember(doc, 0, {"tab_titles", 10});
    if (titles != kPrJsonMissing) {
        if (prJsonKind(doc, titles) != PrJsonKind_Array || prJsonCount(doc, titles) > 1024) return false;
        for (uint32_t i = 0; i < prJsonCount(doc, titles); ++i) {
            auto text = stringAt(doc, prJsonElement(doc, titles, i));
            if (text.size() > 512) return false;
            surface.tabTitles.emplace_back(text);
        }
    }
    const auto items = prJsonMember(doc, 0, {"account_items", 13});
    if (items == kPrJsonMissing) return true;
    if (prJsonKind(doc, items) != PrJsonKind_Array || prJsonCount(doc, items) > 1024) return false;
    surface.items.emplace();
    for (uint32_t i = 0; i < prJsonCount(doc, items); ++i) {
        const auto row = prJsonElement(doc, items, i);
        AccountItem item;
        item.text = stringAt(doc, prJsonMember(doc, row, {"text", 4}));
        item.action = stringAt(doc, prJsonMember(doc, row, {"action", 6}));
        if (item.text.empty() || item.text.size() > 1024 || (!item.action.empty() && !accountAction(item.action)) ||
                !readFlag(doc, row, {"selected", 8}, item.selected)) return false;
        surface.items->push_back(std::move(item));
    }
    return true;
}

bool readWindow(const std::string& answer, std::string_view tab, bool promptOpen, MessageCardStyle& card,
    TranscriptStyle& transcript, std::vector<Shortcut>& keys, std::vector<Panel>& panels, std::map<std::string, TabType>& types,
    std::vector<TabGroup>& groups, std::optional<bool>& closable, std::string& label, std::string& autoLabel, std::string& heading, AccountSurface& surface, std::string& accountLabel, std::string& modelLabel, float& overlayOpacity, bool& centered, bool& signInOnSelect, std::string& why) {
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
    const auto title = stringAt(d, prJsonMember(d, 0, {"sign_in_heading", 15}));
    heading = title.empty() ? "Please sign in" : title;
    if (!readSurface(d, surface)) {
        why = "invalid tab_style, prompt_style, window flag, tab_titles or account_items; check the client window contract in docs/policy.md";
        return false;
    }
    centered = signInOnSelect = false;
    if (!readFlag(d, 0, {"centered_account_menu", 21}, centered) ||
        !readFlag(d, 0, {"sign_in_on_select", 17}, signInOnSelect)) {
        why = "centered_account_menu and sign_in_on_select must be booleans";
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
        else if (name == "limit_choice" && tab == "friend") {
            label = stringAt(d, prJsonMember(d, 0, {"label", 5}));
            accountLabel = stringAt(d, prJsonMember(d, 0, {"limit_account_label", 19}));
            modelLabel = stringAt(d, prJsonMember(d, 0, {"limit_model_label", 17}));
            if (label.empty() || label.size() > 512 || accountLabel.size() > 256 || modelLabel.size() > 256) {
                why = "limit_choice needs a question of up to 512 bytes and button labels of up to 256 bytes";
                return false;
            }
            panel = Panel::LimitChoice;
        }
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

PrAppHotkey overlayBinding(policy::Host& host, std::string& why) {
    const auto answer = host.decide({}, "app.overlay", "{}", why);
    if (answer.empty() || answer == "null") return style::kOverlayHotkey;
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> doc(prJsonCreate(), prJsonDestroy);
    if (prJsonParse(doc.get(), {answer.data(), answer.size()}, nullptr)) {
        const auto code = stringAt(doc.get(), prJsonMember(doc.get(), 0, {"code", 4}));
        const auto mods = stringAt(doc.get(), prJsonMember(doc.get(), 0, {"modifiers", 9}));
        PrAppModifierFlags flags = 0;
        PrAppKey key{};
        if (!mods.empty() && chord(std::string(mods) + "+A", key, flags))
            for (unsigned i = 1; i < PrAppKeyCode_Count; ++i) {
                const auto physical = static_cast<PrAppKeyCode>(i);
                const auto name = prAppKeyCodeName(physical);
                if (name.data && code == std::string_view(name.data, name.size)) return {physical, flags};
            }
    }
    why = "app.overlay needs a physical key code and modifiers such as Command or Command+Shift";
    return style::kOverlayHotkey;
}

void WindowApplication::update(policy::Host& host, const char* tab, const std::string& type, bool promptOpen,
    bool hasStatus, std::size_t activeTab, std::size_t tabCount, const std::string& group, SignInAccount account, bool pickerOpen, LimitOffer offer, const std::string& ui) {
    std::string escapedGroup;
    for (const char c : group) {
        if (c == '\\' || c == '"') escapedGroup += '\\';
        escapedGroup += c;
    }
    auto context = std::string("{\"tab\":\"") + tab + "\",\"type\":\"" + type + "\",\"prompt_open\":" + (promptOpen ? "true" : "false") +
        ",\"has_status\":" + (hasStatus ? "true" : "false") + ",\"active_tab\":" + std::to_string(activeTab) +
        ",\"account_picker_open\":" + (pickerOpen ? "true" : "false") +
        ",\"needs_sign_in\":" + (account.name.empty() ? "false" : "true") + ",\"tab_count\":" + std::to_string(tabCount) + ",\"group\":\"" + escapedGroup + "\"}";
    context.pop_back();
    context += ",\"ui\":" + ui + "}";
    const bool sameContext = context == lastContext_ && account == signInAccount && offer == limitOffer;
    const auto generation = host.refresh();
    if (sameContext && generation == generation_) return;
    lastContext_ = context;
    generation_ = generation;
    signInAccount = std::move(account);
    limitOffer = std::move(offer);
    context.pop_back();
    if (limitOffer.request) {
        std::unique_ptr<PrJsonWriter, decltype(&prJsonWriterDestroy)> json(prJsonWriterCreate(), prJsonWriterDestroy);
        prJsonWriterBeginObject(json.get());
        prJsonWriterKey(json.get(), {"account", 7});
        prJsonWriterString(json.get(), {limitOffer.account.data(), limitOffer.account.size()});
        prJsonWriterKey(json.get(), {"model", 5});
        prJsonWriterString(json.get(), {limitOffer.model.data(), limitOffer.model.size()});
        prJsonWriterKey(json.get(), {"waiting", 7});
        prJsonWriterBool(json.get(), limitOffer.waiting);
        prJsonWriterKey(json.get(), {"connected", 9});
        prJsonWriterBool(json.get(), limitOffer.connected);
        prJsonWriterEndObject(json.get());
        const auto text = prJsonWriterText(json.get());
        context += ",\"limit_offer\":";
        context.append(text.data, text.size);
    }
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
        field({"provider", 8}, signInAccount.provider);
        field({"friend", 6}, signInAccount.friendName);
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
    std::string caption, autoLabel, heading, accountLabel, modelLabel;
    AccountSurface uiSurface;
    bool centered = false, autoSignIn = false;
    float opacity = style::kOverlayOpacity;
    const auto answer = host.decide({}, "app.window", context, why);
    if (why.empty() && readWindow(answer, tab, promptOpen, card, transcript, keys, parts, registrations, layout, mayClose, caption, autoLabel, heading, uiSurface, accountLabel, modelLabel, opacity, centered, autoSignIn, why)) {
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
        signInHeading = std::move(heading);
        surface = std::move(uiSurface);
        limitAccountLabel = std::move(accountLabel);
        limitModelLabel = std::move(modelLabel);
        centeredAccounts = centered;
        signInOnSelect = autoSignIn;
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
        if (!readWindow(fallback, tab, promptOpen, card, transcript, keys, parts, registrations, layout, mayClose, caption, autoLabel, heading, uiSurface, accountLabel, modelLabel, opacity, centered, autoSignIn, why)) {
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
        signInHeading = std::move(heading);
        surface = std::move(uiSurface);
        limitAccountLabel = std::move(accountLabel);
        limitModelLabel = std::move(modelLabel);
        centeredAccounts = centered;
        signInOnSelect = autoSignIn;
        overlayOpacity = opacity;
        messageCard = card;
        transcriptStyle = transcript;
    }
}
} // namespace nexus
