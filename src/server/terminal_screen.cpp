#include "server/terminal_screen.h"

#include <cstring>

#include "sync/terminal_state.h"

namespace nexus::server {

namespace {

constexpr std::size_t kMostParams = 64;

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
        cp = 0xfffd;
    }
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    } else {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
}

// The pen of a cell: everything but its text.
struct Pen {
    PrVtermCellFlags flags = 0;
    std::uint32_t underline = 0;
    PrVtermColor foreground{};
    PrVtermColor background{};
    PrVtermColor underlineColor{};

    bool operator==(const Pen& x) const {
        return flags == x.flags && underline == x.underline &&
            std::memcmp(&foreground, &x.foreground, sizeof foreground) == 0 &&
            std::memcmp(&background, &x.background, sizeof background) == 0 &&
            std::memcmp(&underlineColor, &x.underlineColor, sizeof underlineColor) == 0;
    }
};

constexpr PrVtermCellFlags kStyleFlags = PrVtermCellFlags(~PrVtermCellFlag_WidePlacement);

Pen penOf(const PrVtermCell& c) {
    return Pen{PrVtermCellFlags(c.flags & kStyleFlags), c.underline, c.foreground, c.background, c.underlineColor};
}

// A colour's SGR parameters: `base` 30 (foreground), 40 (background) or 0 (underline colour, 58 only).
void color(std::string& out, const PrVtermColor& c, int base) {
    if (c.kind == PrVtermColorKind_Palette) {
        const unsigned i = prVtermColorIndex(c);
        if (base != 0 && i < 8) {
            out += ';' + std::to_string(base + int(i));
        } else if (base != 0 && i < 16) {
            out += ';' + std::to_string(base + 60 + int(i) - 8);
        } else {
            out += ';' + std::to_string(base == 0 ? 58 : base + 8) + ";5;" + std::to_string(i);
        }
    } else if (c.kind == PrVtermColorKind_Rgb) {
        out += ';' + std::to_string(base == 0 ? 58 : base + 8) + ";2;" + std::to_string(c.red) + ';' +
            std::to_string(c.green) + ';' + std::to_string(c.blue);
    }
}

void sgr(std::string& out, const Pen& p) {
    out += "\x1b[0";
    if (p.flags & PrVtermCellFlag_Bold) out += ";1";
    if (p.flags & PrVtermCellFlag_Faint) out += ";2";
    if (p.flags & PrVtermCellFlag_Italic) out += ";3";
    if (p.underline == PrVtermUnderline_Single) out += ";4";
    else if (p.underline != PrVtermUnderline_None) out += ";4:" + std::to_string(p.underline);
    if (p.flags & PrVtermCellFlag_Blink) out += ";5";
    if (p.flags & PrVtermCellFlag_Inverse) out += ";7";
    if (p.flags & PrVtermCellFlag_Invisible) out += ";8";
    if (p.flags & PrVtermCellFlag_Strikethrough) out += ";9";
    if (p.flags & PrVtermCellFlag_Overline) out += ";53";
    color(out, p.foreground, 30);
    color(out, p.background, 40);
    color(out, p.underlineColor, 0);
    out += 'm';
}

bool blank(const PrVtermCell& c) {
    return (c.codePoint == 0 || c.codePoint == ' ') && c.clusterCount == 0 && penOf(c) == Pen{};
}

} // namespace

void TerminalModes::feed(std::string_view bytes) {
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == 0x18 || c == 0x1a) {     // CAN, SUB: any sequence is cancelled
            state_ = State::Ground;
            continue;
        }
        switch (state_) {
            case State::Ground:
                if (c == 0x1b) state_ = State::Escape;
                break;
            case State::Escape:
                state_ = State::Ground;
                if (c == '[') {
                    params_.clear();
                    state_ = State::Csi;
                } else if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') {
                    state_ = State::String;
                } else if (c == '=') {
                    set_.insert(66);
                } else if (c == '>') {
                    set_.erase(66);
                } else if (c == 'c') {
                    set_.clear();
                } else if (c == 0x1b) {
                    state_ = State::Escape;
                }
                break;
            case State::Csi:
                if (c == 0x1b) {
                    state_ = State::Escape;
                } else if (c >= 0x40 && c <= 0x7e) {
                    finishCsi(char(c));
                    state_ = State::Ground;
                } else if (c >= 0x20 && c <= 0x3f) {
                    if (params_.size() < kMostParams) {
                        params_ += char(c);
                    } else {
                        params_ = "#";      // too long to be a mode: read to its end and dropped
                    }
                }
                break;
            case State::String:
                if (c == 0x07) state_ = State::Ground;
                else if (c == 0x1b) state_ = State::StringEscape;
                break;
            case State::StringEscape:
                state_ = c == '\\' ? State::Ground : State::String;
                break;
        }
    }
}

void TerminalModes::finishCsi(char final) {
    if (final == 'p' && params_ == "!") {   // DECSTR
        set_.clear();
        return;
    }
    if ((final != 'h' && final != 'l') || params_.empty() || params_[0] != '?') {
        return;
    }
    std::uint32_t value = 0;
    bool digits = false;
    for (std::size_t i = 1; i <= params_.size(); ++i) {
        const char c = i < params_.size() ? params_[i] : ';';
        if (c == ';') {
            if (digits && sync::isInputMode(value)) {
                if (final == 'h') set_.insert(value);
                else set_.erase(value);
            }
            value = 0;
            digits = false;
        } else if (c >= '0' && c <= '9') {
            value = value < 100000 ? value * 10 + std::uint32_t(c - '0') : value;
            digits = true;
        } else {
            return;     // an intermediate byte: some other sequence
        }
    }
}

std::string TerminalModes::text() const {
    std::string out;
    for (std::uint32_t m : set_) {
        if (!out.empty()) out += ',';
        out += std::to_string(m);
    }
    return out;
}

std::string paintRow(const PrVterm* vterm, std::uint32_t row) {
    const PrUVec2 extent = prVtermExtent(vterm);
    if (row >= extent.y) {
        return {};
    }
    const PrVtermLine line = prVtermLine(vterm, prVtermScrollbackCount(vterm) + row);
    std::uint32_t count = std::min(line.cellCount, extent.x);
    while (count > 0 && blank(line.cells[count - 1])) {
        --count;
    }
    std::string out;
    Pen pen;
    for (std::uint32_t i = 0; i < count; ++i) {
        const PrVtermCell& c = line.cells[i];
        if (c.flags & (PrVtermCellFlag_WideTail | PrVtermCellFlag_WideSpacer)) {
            continue;
        }
        const Pen p = penOf(c);
        if (!(p == pen)) {
            sgr(out, p);
            pen = p;
        }
        appendUtf8(out, c.codePoint == 0 ? ' ' : c.codePoint);
        for (std::uint32_t k = 0; k < c.clusterCount && line.codePoints != nullptr; ++k) {
            appendUtf8(out, line.codePoints[c.clusterStart + k]);
        }
    }
    if (!(pen == Pen{})) {
        out += "\x1b[0m";
    }
    return out;
}

} // namespace nexus::server
