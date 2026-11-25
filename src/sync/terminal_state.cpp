#include "sync/terminal_state.h"

#include <algorithm>

#include "sync/terminal_paint.h"

namespace nexus::sync {

bool TerminalView::operator==(const TerminalView& x) const {
    if (id != x.id || opener != x.opener || title != x.title || columns != x.columns || rows != x.rows ||
            cursorColumn != x.cursorColumn || cursorRow != x.cursorRow || cursorHidden != x.cursorHidden ||
            modes != x.modes || ended != x.ended || lines.size() != x.lines.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] != x.lines[i] && *lines[i] != *x.lines[i]) {
            return false;
        }
    }
    return true;
}

bool isInputMode(std::uint32_t mode) {
    return std::find(std::begin(kInputModes), std::end(kInputModes), mode) != std::end(kInputModes);
}

bool parseModes(std::string_view text, std::vector<std::uint32_t>* out) {
    out->clear();
    if (text.empty()) {
        return true;
    }
    std::uint32_t value = 0;
    bool digits = false;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',') {
            if (!digits || !isInputMode(value) || (!out->empty() && out->back() >= value)) {
                out->clear();
                return false;
            }
            out->push_back(value);
            value = 0;
            digits = false;
        } else if (text[i] >= '0' && text[i] <= '9' && value < 100000) {
            value = value * 10 + std::uint32_t(text[i] - '0');
            digits = true;
        } else {
            out->clear();
            return false;
        }
    }
    return true;
}

TerminalFate terminalFate(TerminalId id, std::uint64_t heldEpoch, std::uint64_t epoch,
                          const std::vector<TerminalRef>& terminals) {
    for (const TerminalRef& t : terminals) {
        if (t->id == id) {
            return t->ended.empty() ? TerminalFate::Running : TerminalFate::Ended;
        }
    }
    // Not in the state: closed in this run of the server, or lost with the run that held it.
    return heldEpoch != epoch ? TerminalFate::Restarted : TerminalFate::Closed;
}

namespace {

void number(std::string& out, std::uint32_t n) {
    out += std::to_string(n);
}

void moveTo(std::string& out, std::uint32_t row, std::uint32_t column) {
    out += "\x1b[";
    number(out, row + 1);
    out += ';';
    number(out, column + 1);
    out += 'H';
}

} // namespace

bool cleanRow(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b) {
            if (i + 1 >= text.size() || text[i + 1] != '[') {
                return false;
            }
            std::size_t k = i + 2;
            while (k < text.size() && ((text[k] >= '0' && text[k] <= '9') || text[k] == ';' || text[k] == ':')) {
                ++k;
            }
            if (k >= text.size() || text[k] != 'm') {
                return false;
            }
            i = k;
            continue;
        }
        // C0 controls, DEL and the C1 controls' UTF-8 form (U+0080 to U+009F: C2 80 to C2 9F) are refused.
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        if (c == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) < 0xa0) {
            return false;
        }
    }
    return true;
}

std::string paintTerminal(const TerminalView* held, const TerminalView& now, std::uint32_t columns,
                          std::uint32_t rows) {
    std::string out;
    const bool whole = held == nullptr || held->columns != now.columns || held->rows != now.rows ||
        held->lines.size() != now.lines.size();
    if (whole) {
        out += "\x1b[?7l\x1b[0m\x1b[H\x1b[2J\x1b[3J";
    }
    const std::size_t shown = std::min<std::size_t>(now.lines.size(), rows);
    for (std::size_t i = 0; i < shown; ++i) {
        const LineRef& line = now.lines[i];
        if (!whole && (held->lines[i] == line || *held->lines[i] == *line)) {
            continue;
        }
        moveTo(out, std::uint32_t(i), 0);
        out += "\x1b[0m\x1b[2K";
        if (cleanRow(*line)) {
            out += *line;
        }
        out += "\x1b[0m";
    }
    std::vector<std::uint32_t> before;
    std::vector<std::uint32_t> after;
    if (whole || held == nullptr || !parseModes(held->modes, &before)) {
        before.clear();
    }
    if (!parseModes(now.modes, &after)) {
        after.clear();
    }
    for (std::uint32_t mode : kInputModes) {
        const bool was = std::find(before.begin(), before.end(), mode) != before.end();
        const bool is = std::find(after.begin(), after.end(), mode) != after.end();
        if (was != is || whole) {
            out += "\x1b[?";
            number(out, mode);
            out += is ? 'h' : 'l';
        }
    }
    if (columns > 0 && rows > 0) {
        moveTo(out, std::min(now.cursorRow, rows - 1), std::min(now.cursorColumn, columns - 1));
    }
    out += now.cursorHidden || !now.ended.empty() ? "\x1b[?25l" : "\x1b[?25h";
    return out;
}

} // namespace nexus::sync
