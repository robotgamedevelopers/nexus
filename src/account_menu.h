// The account menu that drops from an AI's tab, and the rows of the /account, /login and /logout dialogs (docs/accounts.md,
// "Switching accounts"). Pure logic: the rows from the server's roster, the menu's place under the tab's highlight, and
// the one gesture that opens it and chooses from it. No Prax, no clock, no window: the view draws what this lays out,
// and the frame host feeds it the pointer (frame_host.cpp routeAccountMenu). models/AccountMenu.tla is the gesture's
// model; tests/window/account_menu_walk.cpp binds this code to it by its traces.
//
// The gesture, the owner's (2026-11-25): a press on the active AI's tab opens the menu at once, directly under the tab's
// highlight; the finger slides down onto an account and lifts to choose it. Lifting anywhere that is not a row closes
// the menu and changes nothing: no menu is left open for a tap. A movement from the press that is mostly sideways, past
// the tab-move threshold, closes the menu and becomes the tab's move, as a drag of a tab always was. The first movement
// past the threshold decides: once it went down onto the menu, the press is the menu's until the lift, and a slide across
// a wide row never moves the tab. A press on another tab never opens it. Nothing here changes how the tab or its label
// is drawn.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sync/sessions.h"

namespace nexus {

// The text of the menu's and the /account dialog's last row, which runs /login.
inline constexpr std::string_view kSignInAnotherRow = "Sign in to another account\xE2\x80\xA6";

// One row: an account the AI can switch to at once (signed in), or the row that signs in another.
struct AccountMenuRow {
    std::string account;    // the account's name on the server; empty for the sign-in row
    std::string text;       // its email, else its label, else its name; the sign-in row's text
    bool current = false;   // the account the AI's session is on now
    bool signIn = false;    // the "Sign in to another account…" row
    bool operator==(const AccountMenuRow&) const = default;
};

// The name a person knows an account by: its email, else its label, else its name.
inline std::string accountShownAs(const sync::AccountChoice& a) {
    if (!a.email.empty()) return a.email;
    return a.label.empty() ? a.name : a.label;
}

// Whether a roster entry is an account a person can pick: not the automatic mode, not a command.
inline bool pickable(const sync::AccountChoice& a) {
    return !a.name.empty() && a.name != sync::kAutoAccountTarget && a.name.front() != '+';
}

// The menu's and the /account dialog's rows: the AI's signed-in accounts in the server's order, the current one marked,
// then the row that signs in another account (/login). An account that needs signing in is not here: /login lists it.
inline std::vector<AccountMenuRow> accountMenuRows(const sync::ServerAI& roster, const std::string& current) {
    std::vector<AccountMenuRow> rows;
    for (const auto& a : roster.accounts) {
        if (!pickable(a) || a.needsSignIn) continue;
        rows.push_back({a.name, accountShownAs(a), a.name == current, false});
    }
    rows.push_back({{}, std::string(kSignInAnotherRow), false, true});
    return rows;
}

// The /login card shows every linked account in server order, including saved logins that can be switched to.
inline std::vector<AccountMenuRow> accountLoginRows(const sync::ServerAI& roster) {
    std::vector<AccountMenuRow> rows;
    for (const auto& a : roster.accounts) {
        if (pickable(a)) rows.push_back({a.name, accountShownAs(a), false, false});
    }
    return rows;
}

// Where an AI goes after /logout of `leaving`: the next signed-in account after it in the server's order, wrapping;
// none (empty) when no other is signed in, and the AI then shows the sign-in prompt.
inline std::string accountAfterLogout(const sync::ServerAI& roster, const std::string& leaving) {
    const auto& list = roster.accounts;
    const auto at = std::find_if(list.begin(), list.end(), [&](const auto& a) { return a.name == leaving; });
    const std::size_t start = at == list.end() ? 0 : std::size_t(at - list.begin()) + 1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& a = list[(start + i) % list.size()];
        if (pickable(a) && !a.needsSignIn && a.name != leaving) return a.name;
    }
    return {};
}

// A rectangle in the window's viewport, in points (layout px).
struct MenuBox {
    float left = 0, top = 0, right = 0, bottom = 0;
    float width() const { return right - left; }
    float height() const { return bottom - top; }
    bool contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
    bool overlaps(const MenuBox& o) const { return left < o.right && o.left < right && top < o.bottom && o.top < bottom; }
    bool operator==(const MenuBox&) const = default;
};

// The menu's look and measures, from the client policy's account_menu (policy/rules/config.json, docs/policy.md).
struct AccountMenuStyle {
    float rowHeight = 44;           // points: a finger's row; never less than kMinRowHeight
    float gap = 4;                  // points between the bottom of the tab's highlight and the menu's top
    float paddingX = 14;            // points either side of a row's text
    float radius = 10;              // the menu's corners
    float maxWidthEm = 22;          // the widest the menu grows, in the tab label's em
    float tabMoveThreshold = 12;    // points sideways from the press past which the press is the tab's move
    float dialogTextEm = 16;        // the widest a row's text is in the /account and /login dialogs, in their text's em
    unsigned fill = 0x2a2a2a, ink = 0xffffff, hoverFill = 0x86c5ff, markInk = 0x86c5ff;
    float hoverOpacity = 0.5f;
    std::string mark = "\xE2\x9C\x93";   // shown before the current account
    bool operator==(const AccountMenuStyle&) const = default;
};

// Apple's smallest comfortable target (Human Interface Guidelines): no row is shorter, whatever the policy says.
inline constexpr float kMinRowHeight = 44;

// The menu laid out under a tab's highlight.
struct AccountMenuLayout {
    MenuBox menu;
    std::vector<MenuBox> rows;
    float textMax = 0;      // the widest a row's text may be before it is cut with "…"
    std::optional<std::size_t> rowAt(float x, float y) const {
        if (!menu.contains(x, y)) return std::nullopt;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].contains(x, y)) return i;
        }
        return std::nullopt;
    }
};

// The menu under `highlight` (the active tab's highlight shape): its top just below the highlight by the style's gap,
// its left edge on the highlight's left edge, one row per row of `count`, each style.rowHeight (at least 44) tall. Its
// width is the widest row's text (`widest`, in points, the mark's column included) with the padding, at least the
// highlight's, at most `em` x maxWidthEm and never past the window's right edge less the same gap; a row's text wider
// than that is cut (ellipsize).
inline AccountMenuLayout layoutAccountMenu(const MenuBox& highlight, std::size_t count, float widest, float em,
                                           float windowWidth, const AccountMenuStyle& style) {
    AccountMenuLayout out;
    const float rowHeight = std::max(style.rowHeight, kMinRowHeight);
    const float top = highlight.bottom + std::max(style.gap, 0.0f);
    const float left = highlight.left;
    float width = std::max(widest + 2 * style.paddingX, highlight.width());
    width = std::min(width, std::max(em * style.maxWidthEm, highlight.width()));
    width = std::max(std::min(width, windowWidth - left - std::max(style.gap, 0.0f)), 0.0f);
    out.menu = {left, top, left + width, top + rowHeight * float(count)};
    for (std::size_t i = 0; i < count; ++i) {
        const float y = top + rowHeight * float(i);
        out.rows.push_back({left, y, left + width, y + rowHeight});
    }
    out.textMax = std::max(width - 2 * style.paddingX, 0.0f);
    return out;
}

// `text` cut to fit `room` points as `measure` measures it, ending in "…" when cut: never wrapped. A cut falls between
// whole UTF-8 characters.
inline std::string ellipsize(const std::string& text, float room, const std::function<float(std::string_view)>& measure,
                             std::string_view marker = "…") {
    if (measure(text) <= room) return text;
    if (measure(marker) > room) return {};
    std::vector<std::size_t> cuts;     // the byte offsets that start a character, past the first
    for (std::size_t i = 1; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
    }
    cuts.push_back(text.size());
    std::size_t lo = 0, hi = cuts.size();     // the longest prefix that fits, by binary search over the cuts
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        std::string candidate = text.substr(0, cuts[mid]);
        candidate += marker;
        if (measure(candidate) <= room) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return std::string(marker);
    std::string out = text.substr(0, cuts[lo - 1]);
    out += marker;
    return out;
}

// The one gesture of the menu: closed; open while the finger that opened it is down (the menu shows, the row under the
// finger lit); or the finger's movement became the tab's move. Each input says what the host does with the event.
class AccountMenuGesture {
public:
    enum class Phase : std::uint8_t { Closed, Open, TabMove };

    struct Step {
        bool consumed = false;          // the event is the menu's: the tab strip never sees it
        bool opened = false;            // the menu opened with this event
        bool closed = false;            // the menu closed with this event
        bool startTabMove = false;      // the strip is to get the press at the origin and this movement, as a drag
        std::optional<std::size_t> chosen;  // the row lifted on: choose it
        bool operator==(const Step&) const = default;
    };

    explicit AccountMenuGesture(float threshold = 12) : threshold_(threshold) {}

    Phase phase() const { return phase_; }
    bool open() const { return phase_ == Phase::Open; }
    std::optional<std::size_t> hovered() const { return hovered_; }
    float originX() const { return ox_; }
    float originY() const { return oy_; }
    void setThreshold(float t) { threshold_ = t; }

    // A press. `onActiveAnchor`: it lands on the active AI's tab (or the iPhone's name at the top of an AI's page), and
    // that AI has accounts to show. Only that press opens the menu, and it is the menu's alone.
    Step down(float x, float y, bool onActiveAnchor) {
        Step s;
        decided_ = false;
        if (phase_ == Phase::Open) {        // a second finger, or a press the host missed the release of: closes
            phase_ = Phase::Closed;
            hovered_.reset();
            s.closed = true;
            s.consumed = true;
            return s;
        }
        phase_ = Phase::Closed;
        if (!onActiveAnchor) return s;
        phase_ = Phase::Open;
        ox_ = x;
        oy_ = y;
        hovered_.reset();
        s.opened = s.consumed = true;
        return s;
    }

    // The finger moved. Open and not yet decided: mostly sideways past the threshold, the menu closes and the press
    // becomes the tab's move; mostly down past it, the press is the menu's from then on (decided). Either way, while
    // open, the row under the finger is lit.
    Step move(float x, float y, const AccountMenuLayout& layout) {
        Step s;
        if (phase_ != Phase::Open) return s;
        const float dx = std::fabs(x - ox_), dy = std::fabs(y - oy_);
        if (!decided_ && dx > threshold_ && dx > dy) {
            phase_ = Phase::TabMove;
            hovered_.reset();
            s.closed = true;
            s.startTabMove = true;
            return s;
        }
        if (dy > threshold_ && dy >= dx) decided_ = true;
        hovered_ = layout.rowAt(x, y);
        s.consumed = true;
        return s;
    }

    // Whether the first movement past the threshold went down onto the menu: the press is the menu's until the lift.
    bool decided() const { return decided_; }

    // The finger lifted. Open: on a row, that row is chosen; anywhere else nothing changes. Either way the menu closes.
    Step up(float x, float y, const AccountMenuLayout& layout) {
        Step s;
        if (phase_ == Phase::TabMove) {
            phase_ = Phase::Closed;
            return s;
        }
        if (phase_ != Phase::Open) return s;
        phase_ = Phase::Closed;
        hovered_.reset();
        s.chosen = layout.rowAt(x, y);
        s.closed = s.consumed = true;
        return s;
    }

    // The system took the pointer: the menu closes with no choice.
    Step cancel() {
        Step s;
        if (phase_ == Phase::Open) s.closed = s.consumed = true;
        phase_ = Phase::Closed;
        hovered_.reset();
        return s;
    }

private:
    Phase phase_ = Phase::Closed;
    float ox_ = 0, oy_ = 0;
    float threshold_;
    bool decided_ = false;
    std::optional<std::size_t> hovered_;
};

} // namespace nexus
