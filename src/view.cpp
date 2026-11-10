#include "view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

#include "pr/kit_choice.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_text.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/ui_records.h"
#include "pr/ui_text_edit.h"

#include "style.h"

namespace nexus {

namespace {

using namespace style;

// ---------------------------------------------------------------------------------------------------------------------
// Records for what no kit part draws: the window's frame and background, the transcript's scroller and column, the chat
// bubbles, the prompt's rounded box, and the rows that place the kit's parts. One part per kind of node, and one recipe
// per group kind that switches on the part. Their text is the kit's (labels in the theme's roles), so no record here
// sets a font.

enum class Part : unsigned char {
    Window,
    Transcript,
    Column,
    Centered,
    Entry,
    Bubble,
    BubbleQueued,
    Footer,
    Prompt,
    StripRow,
    BoardBox,
};

// The height of the kit text area with one line at text scale `k`, which the plus beside it centres on: the line, the
// area's padding of 4 px above and below (kit_input.cpp kAreaPadding), and its 1 px borders, which do not scale. The
// numbers are the kit's own, copied: Prax has no public query for a text area's height or padding (a request).
float oneLineArea(float k) {
    return kLinePitch * k + 2 * 4 + 2;
}

// The prompt box's padding above and below the text area, inside its 1 px border, so that with one line it is
// kPromptHeight tall.
float promptPadY(float k) {
    return (kPromptHeight * k - oneLineArea(k)) / 2 - 1;
}

PrColor faded(PrColor c, float alpha) {
    c.a = alpha;
    return c;
}

PrLayoutLength px(float v) {
    return prLayoutLengthPx(v);
}

void radius(PrLayoutDecorationStyle& d, float r) {
    for (PrLayoutLength& l : d.radii)
        l = px(r);
}

void padding(PrLayoutBoxStyle& box, float top, float right, float bottom, float left) {
    box.padding[PrRectSide_Top] = px(top);
    box.padding[PrRectSide_Right] = px(right);
    box.padding[PrRectSide_Bottom] = px(bottom);
    box.padding[PrRectSide_Left] = px(left);
}

// Each recipe holds its part and the text scale, in steps from 1 (style::textScale), so a new scale makes new records.
struct BoxRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    Part part;
    signed char steps;

    void make(const Look&, PrLayoutBoxStyle& box) const {
        const float k = textScale(steps);
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        switch (part) {
            case Part::Window:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(100);
                box.height = prLayoutLengthPercent(100);
                break;
            case Part::Transcript:
                box.display = PrLayoutDisplay_Flex;
                box.minHeight = px(0);
                box.overflowX = PrLayoutOverflow_Hidden;
                box.overflowY = PrLayoutOverflow_Scroll;
                break;
            case Part::Column:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kColumnPercent);
                box.minHeight = prLayoutLengthPercent(100);
                box.margin[PrRectSide_Left] = prLayoutLengthAuto();
                box.margin[PrRectSide_Right] = prLayoutLengthAuto();
                padding(box, kColumnInset * k, 0, 0, 0);
                break;
            case Part::Centered:
                box.display = PrLayoutDisplay_Block;
                box.margin[PrRectSide_Top] = prLayoutLengthAuto();
                box.margin[PrRectSide_Bottom] = prLayoutLengthAuto();
                break;
            case Part::Entry:
                box.display = PrLayoutDisplay_Flex;
                break;
            case Part::Bubble:
            case Part::BubbleQueued:
                box.display = PrLayoutDisplay_Block;
                box.maxWidth = prLayoutLengthPercent(kBubbleMaxPercent);
                padding(box, kBubblePadY * k, kBubblePadX * k, kBubblePadY * k, kBubblePadX * k);
                break;
            case Part::Footer:
                box.display = PrLayoutDisplay_Flex;
                padding(box, kU * k, 0, kU * k, 0);
                break;
            case Part::Prompt:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kPromptPercent);
                padding(box, promptPadY(k), kPromptPadX * k, promptPadY(k), kPromptPadX * k);
                for (PrLayoutUnit& b : box.border)
                    b = prLayoutUnitFromPx(1);
                break;
            case Part::StripRow:
                box.display = PrLayoutDisplay_Flex;
                box.height = px(kStripHeight * k + 1);
                break;
            case Part::BoardBox:
                box.display = PrLayoutDisplay_Flex;
                box.minHeight = px(0);
                box.overflowX = PrLayoutOverflow_Hidden;
                box.overflowY = PrLayoutOverflow_Hidden;
                break;
        }
    }
};

struct DecorationRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    Part part;
    signed char steps;

    void make(const Look& look, PrLayoutDecorationStyle& d) const {
        const float k = textScale(steps);
        switch (part) {
            case Part::Window:
                d.background = prColor16FromColor(look.overlay ? faded(kBackground, kOverlayOpacity) : kBackground);
                break;
            case Part::Bubble:
                d.background = prColor16FromColor(kBubble);
                radius(d, kBubbleRadius * k);
                break;
            case Part::BubbleQueued:
                d.background = prColor16FromColor(kQueuedBubble);
                radius(d, kBubbleRadius * k);
                break;
            case Part::Prompt:
                d.background = prColor16FromColor(kPromptFill);
                for (int side = 0; side < PrRectSide_Count; ++side) {
                    d.borderColor[side] = prColor16FromColor(kPromptBorder);
                    d.borderStyle[side] = PrDrawBorderStyle_Solid;
                }
                radius(d, kPromptRadius * k);
                break;
            default:
                break;
        }
    }
};

struct FlexRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    Part part;
    signed char steps;

    void make(const Look&, PrLayoutFlexStyle& flex) const {
        const float k = textScale(steps);
        switch (part) {
            case Part::Window:
                flex.direction = PrLayoutFlexDirection_Column;
                break;
            case Part::Transcript:
                // It starts at the newest entry and stays there as entries arrive (prUiFollowEnd, in transcript()), and
                // scroll anchoring holds a view scrolled up in place.
                flex.direction = PrLayoutFlexDirection_Column;
                flex.grow = 1;
                flex.basis = px(0);
                break;
            case Part::Column:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.justifyContent = PrLayoutAlignment_FlexEnd;
                flex.rowGap = px(kGap * k);
                flex.shrink = 0;
                break;
            case Part::Entry:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.alignItems = PrLayoutAlignment_FlexEnd;
                break;
            case Part::Footer:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.alignItems = PrLayoutAlignment_Center;
                flex.shrink = 0;
                break;
            case Part::Prompt:
                flex.direction = PrLayoutFlexDirection_Row;
                flex.alignItems = PrLayoutAlignment_FlexEnd;
                flex.columnGap = px(kPromptGap * k);
                flex.shrink = 0;
                break;
            case Part::StripRow:
                flex.direction = PrLayoutFlexDirection_Row;
                flex.shrink = 0;
                break;
            case Part::BoardBox:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.grow = 1;
                flex.basis = px(0);
                break;
            default:
                break;
        }
    }
};

PrLayoutStyleIndex style(PrUi* ui, const Look& look, Part p) {
    signed char s = static_cast<signed char>(look.textSteps);
    return pr::uiStyle(ui, look, BoxRecipe{p, s}, DecorationRecipe{p, s}, FlexRecipe{p, s});
}

// ---------------------------------------------------------------------------------------------------------------------
// The tree.

PrStringView sv(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

// The kit's id of the tab each strip holds but no round declares (syncTabs).
constexpr uint64_t kNoTab = ~uint64_t(0);

// The digits that select tabs with Command, in tab order: the keyboard's number row.
constexpr std::string_view kTabKeys = "1234567890";

// The text scale's keys, in this one place: a logical key with exactly these modifiers, and the steps it moves the
// scale: the standard macOS keys. Command-= is larger without Shift, as Command-+ (Shift-=) is; Command-minus is smaller.
struct TextKey {
    PrAppKey key;
    PrAppModifierFlags modifiers;
    int steps;
};
constexpr TextKey kTextKeys[] = {
    {'=', PrAppModifierFlag_Super, 1},
    {'+', PrAppModifierFlags(PrAppModifierFlag_Super | PrAppModifierFlag_Shift), 1},
    {'-', PrAppModifierFlag_Super, -1},
};

PrKitTextStyle textStyle(PrKitTextRole role, PrKitTone tone, PrKitTextAlign align = PrKitTextAlign_Start) {
    return PrKitTextStyle{.role = role, .tone = tone, .wrap = PrKitWrap_Auto, .align = align, .preserve = true};
}

struct Builder {
    View& view;
    PrKit* kit;
    PrUi* ui;
    PrLayoutTree* tree;
    const Look& look;
    Model& model;

    // The text scale the look and the kit's theme are at.
    float k() const {
        return textScale(look.textSteps);
    }

    PrLayoutNode element(PrLayoutNode parent, Part p, PrUiFlags flags = 0, PrUiKey key = 0) {
        return prUiElement(ui, parent, key, style(ui, look, p), flags);
    }

    PrLayoutNode label(PrLayoutNode parent, std::string_view text, PrKitTextStyle s) {
        return prKitLabel(kit, parent, PrKitLabel{.key = 0, .text = sv(text)}, s);
    }

    // The kit's plus, centred on the prompt's last line, where the box aligns its items at the bottom.
    void plus(PrLayoutNode box) {
        const float side = kPlusSide * k();
        PrLayoutNode node = prKitIcon(kit, box, PrKitIcon{.key = 0, .icon = PrKitIconKind_Plus},
            PrKitIconStyle{.size = side, .tone = PrKitTone_Primary, ._pad = {}});
        PrKitPlace place{};
        place.margin.bottom = (oneLineArea(k()) - side) / 2;
        place.flex = PrKitFlex_Fixed;
        prKitPlace(kit, node, &place);
    }

    // Takes the kit text area's own frame off: the kit draws a text area with a square frame (3 px corners), a border in
    // the accent while focused and a focus ring, and offers no option to round it, quiet it or leave it out (the bare
    // style is not honoured by text areas), so the prompt's rounded box draws the frame and the area's decoration is
    // the default: no fill, no border, no ring. Its box (rows, padding, scrolling) and all its editing stay the kit's.
    // The public way would be PrKitFieldStyle.bare, which prKitTextArea ignores (kit_input.cpp prKitTextArea sets no
    // PartFlag_Bare): a request for Prax.
    void frameless(PrLayoutNode area) {
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, area));
        record.groups[PrLayoutGroup_Decoration] = 0;
        prLayoutTreeFrameSetStyle(tree, area, prLayoutStyleIntern(tree, &record));
    }

    // The kit's id of a tab: its Tab::id plus one, since the kit's ids are nonzero.
    static uint64_t kitId(const Tab& tab) {
        return uint64_t(tab.id) + 1;
    }

    // Copies the model's tabs at positions `first` to `end` less one, their order and the selection, into the kit's tabs
    // of their strip. The kit selects a tab in every strip that holds one, and has no strip with none selected
    // (kit_choice.h PrKitTabs; kit_choice.cpp prKitTabsInsert 640-647, prKitTabsRemove 649-659): so each strip also
    // holds kNoTab, last, which no round declares, and which is selected while the active tab stands in the other
    // strip, so that only the active tab shows as selected. A request for Prax: a strip with no selected tab.
    void syncTabs(PrKitTabs* tabs, std::size_t first, std::size_t end) {
        for (uint32_t j = prKitTabsCount(tabs); j-- > 0;) {
            const uint64_t id = prKitTabsAt(tabs, j);
            bool held = id == kNoTab;
            for (std::size_t i = first; i < end && !held; ++i)
                held = kitId(model.tab(i)) == id;
            if (!held)
                prKitTabsRemove(tabs, id);
        }
        for (std::size_t i = first; i < end; ++i) {
            const uint32_t at = uint32_t(i - first);
            const uint64_t id = kitId(model.tab(i));
            if (prKitTabsAt(tabs, at) == id)
                continue;
            if (prKitTabsIndex(tabs, id) == prKitTabsCount(tabs))
                prKitTabsInsert(tabs, id, at);
            else
                prKitTabsMove(tabs, tabs, id, at);
        }
        if (prKitTabsIndex(tabs, kNoTab) == prKitTabsCount(tabs))
            prKitTabsInsert(tabs, kNoTab, uint32_t(end - first));
        const std::size_t active = model.active();
        prKitTabsSelect(tabs, active >= first && active < end ? kitId(model.tab(active)) : kNoTab);
    }

    // A drop within a strip is applied by the kit as the strip is declared, and is not reported (kit_choice.cpp
    // prKitTabStrip 738-746; prKitTabMoved reports only moves between strips): it is read back here from the kit's
    // order, and made through the model's move_tab. The model refuses a move across zones (a terminal among the
    // friends, or a friend among the terminals: the kit has no way to keep its caret out of another zone, and no per-tab
    // drop limit), and the kit's tabs are then put back as the model has them, as they are after an accepted move too,
    // so that the drop leaves the active tab as it was. A request for Prax: a report of a move within a strip, and a
    // per-tab drop limit.
    void mirrorMove(PrKitTabs* tabs, std::size_t first, std::size_t end) {
        std::vector<uint64_t> shown;
        for (uint32_t j = 0; j < prKitTabsCount(tabs); ++j) {
            if (prKitTabsAt(tabs, j) != kNoTab)
                shown.push_back(prKitTabsAt(tabs, j));
        }
        const std::size_t n = end - first;
        if (shown.size() != n)
            return;
        std::size_t lo = 0;
        while (lo < n && shown[lo] == kitId(model.tab(first + lo)))
            ++lo;
        if (lo == n) {
            syncTabs(tabs, first, end);
            return;
        }
        std::size_t hi = n - 1;
        while (shown[hi] == kitId(model.tab(first + hi)))
            --hi;
        // One tab moved: from the last differing place to the first, or from the first to the last.
        std::size_t from = hi;
        std::size_t to = lo;
        if (shown[lo] != kitId(model.tab(first + hi))) {
            from = lo;
            to = hi;
        }
        if (model.move_tab(first + from, first + to) && view.moved)
            view.moved();
        syncTabs(tabs, first, end);
    }

    // One of the kit's tab strips, holding the tabs at positions `first` to `end` less one; reorderable or not. In a
    // reorderable strip, a tab pressed and dragged past the kit's threshold is carried, an accent caret marks where it
    // will land, Escape cancels it, and the drop is made through the model (mirrorMove). A press released before the
    // threshold is a click that selects the tab. Each tab is keyed by its Tab::id, so its node (focus, pointer state)
    // goes with it when it moves.
    // The tabs' nodes by position, as the strips declared them this round.
    std::vector<PrLayoutNode> tabNodes;

    void strip(PrLayoutNode parent, PrKitTabs* tabs, std::size_t first, std::size_t end, bool reorderable,
            PrKitFlex flex) {
        syncTabs(tabs, first, end);
        PrLayoutNode row = prKitTabStrip(kit, parent, PrKitTabStrip{.key = 0, .tabs = tabs, .group = 0,
            .reorderable = reorderable, .collapsesSingle = false, .disabled = false},
            PrKitChoiceStyle{.size = PrKitControlSize_Lg, .direction = PrKitDirection_Row});
        PrKitPlace rowPlace{};
        rowPlace.height = prKitPx(kStripHeight * k() + 1);
        rowPlace.flex = flex;
        prKitPlace(kit, row, &rowPlace);
        if (reorderable)
            mirrorMove(tabs, first, end);
        const uint64_t selected = prKitTabsSelected(tabs);
        for (std::size_t i = first; i < end; ++i) {
            PrLayoutNode tab = prKitTab(kit, row, PrKitTab{.id = kitId(model.tab(i)), .disabled = false});
            PrKitPlace tabPlace{};
            tabPlace.height = prKitPx(1.75f * kTabSize * k());
            tabPlace.align = PrKitAlign_Center;
            if (i == 0)
                tabPlace.margin.left = 0.8f * kU * k();
            prKitPlace(kit, tab, &tabPlace);
            prKitTabLabel(kit, tab, PrKitLabel{.key = 0, .text = sv(label(model.tab(i)))});
            tabNodes[i] = tab;
        }
        // A click on a tab, or Left or Right on a focused one, selects in the kit: made through the model's select_tab.
        // Right on the last tab selects kNoTab, which selects nothing: the next round puts the selection back.
        const uint64_t now = prKitTabsSelected(tabs);
        if (now != selected && now != kNoTab)
            model.select_tab(first + prKitTabsIndex(tabs, now));
    }

    // The tab strip. The sprint tab, if there is one, is first, in a strip of its own that is not reorderable: it is
    // never carried, and the other strip's caret never stands before it. The kit has no per-tab way to pin a tab
    // (PrKitTab is only an id and disabled, kit_choice.h 132-135; kit_choice.cpp prKitTab 817-820 starts a drag for every
    // tab of a reorderable strip): a request for Prax. The two strips stand side by side, so their rules meet as one.
    void strips(PrLayoutNode window) {
        const std::size_t pinned = model.tab_count() != 0 && model.tab(0).kind == TabKind::Sprint ? 1 : 0;
        tabNodes.assign(model.tab_count(), kPrLayoutNodeNone);
        if (pinned == 0) {
            strip(window, view.movableTabs.get(), 0, model.tab_count(), true, PrKitFlex_Fixed);
        } else {
            PrLayoutNode row = element(window, Part::StripRow);
            strip(row, view.pinnedTabs.get(), 0, pinned, false, PrKitFlex_Fixed);
            strip(row, view.movableTabs.get(), pinned, model.tab_count(), true, PrKitFlex_Fill);
        }
        keepFocusOnTheActiveTab();
    }

    // A press focuses the tab it lands on (ui.cpp PointerDown), a drag included, and the sprint tab has no prompt to
    // take the focus back: left there, the focus ring stood on the dragged tab once a key made the focus visible (an
    // Escape that hid the overlay), and Enter or Space reaching the tree selected it. So the keyboard focus never rests
    // on a tab other than the active one: it goes to the active tab, and the prompt or a terminal takes it from there
    // later in the round, as each does every round.
    void keepFocusOnTheActiveTab() {
        const std::size_t active = model.active();
        for (std::size_t i = 0; i < tabNodes.size(); ++i) {
            if (i != active && tabNodes[i] != kPrLayoutNodeNone && prUiFocus(ui) == tabNodes[i] &&
                    active < tabNodes.size() && tabNodes[active] != kPrLayoutNodeNone)
                prUiSetFocus(ui, tabNodes[active]);
        }
    }

    // A friend's tab shows the friend's name; the sprint tab "sprint"; a terminal's the title its shell set, or
    // "terminal".
    std::string label(const Tab& tab) {
        if (tab.kind == TabKind::Friend)
            return tab.name;
        if (tab.kind == TabKind::Sprint)
            return "sprint";
        std::string title = view.terminals != nullptr ? view.terminals->title(tab.id) : std::string();
        return title.empty() ? std::string("terminal") : title;
    }

    // Each friend keeps its scroll while another shows, wherever its tab stands: a friend that showed its newest entry
    // (and one never shown) comes back at its newest entry, and one scrolled up comes back at the same offset from the
    // top, where the same entries show, since entries only arrive at the end. Returns false if the transcript should not
    // follow its end this round: it put back a view scrolled up, and the last layout was another friend's.
    bool keepScroll(PrLayoutNode scroller, std::size_t id) {
        if (view.shown == id)
            return true;
        leaveTranscript();
        view.shown = id;
        if (view.scrolls[id] == View::kAtEnd) {
            // An offset past the end, which layout clamps to the end of the content this round makes, as prUiFollowEnd
            // sets it (ui.cpp 2488-2493): prUiFollowEnd itself reads the last layout, which was another friend's.
            prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), PrLayoutUnit{INT32_MAX}});
            return true;
        }
        prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(view.scrolls[id])});
        return false;
    }

    // Keeps the scroll of the friend the transcript showed last round (by Tab::id; the friends' ids are 0 to the number
    // of friends less one, below tab_count()).
    void leaveTranscript() {
        view.scrolls.resize(model.tab_count(), View::kAtEnd);
        PrLayoutScrollInfo info{};
        if (view.shown < view.scrolls.size() && view.transcript != kPrLayoutNodeNone &&
                prLayoutNodeScrollInfo(tree, view.transcript, &info) && info.scrollContainer)
            view.scrolls[view.shown] =
                info.offset.y.raw < info.range.bottom.raw ? prLayoutUnitToPx(info.offset.y) : View::kAtEnd;
    }

    // A terminal tab: its terminal fills the window's width and the height below the strip, with no prompt. The
    // transcript and the prompt are not declared, so the friend shown before keeps its scroll, and its draft comes back
    // with the caret at its end.
    void terminal(PrLayoutNode window, const Tab& tab) {
        if (view.shown != ~std::size_t(0))
            leaveTranscript();
        view.shown = ~std::size_t(0);
        view.prompted = ~std::size_t(0);
        view.transcript = kPrLayoutNodeNone;
        view.prompt = kPrLayoutNodeNone;
        view.promptBox = kPrLayoutNodeNone;
        view.terminals->show(tab.id, window);
    }

    // The sprint tab: its page fills the window's width and the height below the strip, with no prompt; or, while there
    // is no page to show, the line that says why. With no friends, the reason there are none is a faint line beneath.
    // Like a terminal tab, it leaves the friend shown before with its scroll and its draft.
    void sprint(PrLayoutNode window) {
        if (view.shown != ~std::size_t(0))
            leaveTranscript();
        view.shown = ~std::size_t(0);
        view.prompted = ~std::size_t(0);
        view.transcript = kPrLayoutNodeNone;
        view.prompt = kPrLayoutNodeNone;
        view.promptBox = kPrLayoutNodeNone;
        // The box takes the room the strip and any notice leave; the page is laid out at the box's extent from the last
        // layout (the strip's height beneath the window's in the first).
        PrLayoutNode box = element(window, Part::BoardBox);
        PrVec2 extent{view.extent.x, std::max(0.0f, view.extent.y - (kStripHeight * k() + 1))};
        if (view.boardBox != kPrLayoutNodeNone) {
            PrRect last = prLayoutRectToPx(prLayoutNodeBounds(tree, view.boardBox, PrLayoutCoordinates_Layout));
            if (last.right - last.left >= 1 && last.bottom - last.top >= 1)
                extent = PrVec2{last.right - last.left, last.bottom - last.top};
        }
        view.boardBox = box;
        std::string problem = "this build has no sprint board";
        if (view.board == nullptr || !view.board->show(box, extent, &problem)) {
            PrLayoutNode column = element(box, Part::Column);
            PrLayoutNode centered = element(column, Part::Centered);
            label(centered, problem, textStyle(PrKitTextRole_Body, PrKitTone_Muted, PrKitTextAlign_Center));
        }
        bool friends = false;
        for (std::size_t i = 0; i < model.tab_count() && !friends; ++i)
            friends = model.tab(i).kind == TabKind::Friend;
        if (!friends && !view.notice.empty()) {
            PrLayoutNode footer = element(window, Part::Footer);
            label(footer, view.notice, textStyle(PrKitTextRole_BodySmall, PrKitTone_Faint, PrKitTextAlign_Center));
        }
    }

    // Each round: a terminal whose shell ended closes; every other terminal that does not show is serviced, so that
    // its title, alerts and exit stay current.
    void serviceTerminals() {
        if (view.terminals == nullptr)
            return;
        for (std::size_t i = model.tab_count(); i-- > 0;) {
            const Tab& tab = model.tab(i);
            if (tab.kind != TabKind::Terminal)
                continue;
            const std::size_t id = tab.id;
            if (view.terminals->ended(id)) {
                model.close_tab(i);
                view.terminals->close(id);
            } else if (i != model.active()) {
                view.terminals->update(id);
            }
        }
    }

    void transcript(PrLayoutNode window) {
        PrLayoutNode scroller = element(window, Part::Transcript, PrUiFlag_Scrollable);
        PrLayoutNode column = element(scroller, Part::Column);
        if (model.tab_count() == 0) {
            PrLayoutNode centered = element(column, Part::Centered);
            label(centered, view.notice, textStyle(PrKitTextRole_Body, PrKitTone_Faint, PrKitTextAlign_Center));
            view.transcript = scroller;
            return;
        }
        const Tab& tab = model.tab(model.active());
        if (keepScroll(scroller, tab.id))
            prUiFollowEnd(ui, scroller);
        view.transcript = scroller;
        // The kit's labels and text area wrap, breaking inside a word that fits no line (PrKitWrap_Wrap, kit_theme.h
        // 99), so a long URL or token wraps inside its box instead of running past it.
        for (const Entry& e : tab.transcript) {
            switch (e.kind) {
                case EntryKind::Person: {
                    PrLayoutNode entry = element(column, Part::Entry);
                    PrLayoutNode bubble = element(entry, Part::Bubble);
                    label(bubble, e.text, textStyle(PrKitTextRole_Body, PrKitTone_Primary));
                    break;
                }
                case EntryKind::Friend:
                    label(column, e.text, textStyle(PrKitTextRole_Body, PrKitTone_Primary));
                    break;
                case EntryKind::Notice:
                    label(column, e.text, textStyle(PrKitTextRole_Body, PrKitTone_Muted));
                    break;
            }
        }
        // The tab's queue, after its transcript and apart from it: each prompt held for later, oldest first, in a
        // dimmer bubble marked "queued".
        for (const std::string& text : tab.queue) {
            PrLayoutNode entry = element(column, Part::Entry);
            PrLayoutNode bubble = element(entry, Part::BubbleQueued);
            label(bubble, text, textStyle(PrKitTextRole_Body, PrKitTone_Muted));
            PrLayoutNode tag = label(entry, "queued", textStyle(PrKitTextRole_Meta, PrKitTone_Faint));
            PrKitPlace place{};
            place.margin.top = 0.3f * kMetaSize * k();
            place.margin.right = 6 * k();
            prKitPlace(kit, tag, &place);
        }
    }

    // The kit's text area has a fixed number of rows; the prompt grows with its text up to kPromptMaxLines, so the rows
    // come from how tall the text was at the last layout. A workaround for the area's missing auto height. It assumes the
    // area's first child holds its text (kit_input.cpp editedText); Prax has no public auto-height area or query for
    // the area's text element (a request).
    uint32_t promptRows() {
        if (view.prompt == kPrLayoutNodeNone)
            return 1;
        PrLayoutNode text = prLayoutNodeFirstChild(tree, view.prompt);
        if (text == kPrLayoutNodeNone)
            return view.promptRows;
        PrLayoutRect box = prLayoutNodeBounds(tree, text, PrLayoutCoordinates_Layout);
        float lines = std::round(prLayoutUnitToPx(box.bottom - box.top) / (kLinePitch * k()));
        return uint32_t(std::clamp(lines, 1.0f, kPromptMaxLines));
    }

    // `composing`: an input method was composing in the prompt before this round's events (read in build).
    void prompt(PrLayoutNode footer, bool composing) {
        std::size_t active = model.active();
        // The prompt's rounded box, holding the plus and the text area: the kit has no rounded text area and no slot
        // for an icon inside one (frameless). Enter sends and Command-Enter queues, captured on the box, so the area
        // never sees them; every other Enter (Shift-Enter among them) goes to the area, which breaks the line (pr/ui.h).
        PrLayoutNode box = element(footer, Part::Prompt);
        view.promptBox = box;
        bool send = prUiKeyCaptured(ui, box, PrAppKey_Enter, 0);
        bool queue = prUiKeyCaptured(ui, box, PrAppKey_Enter, PrAppModifierFlag_Super);
        view.promptRows = promptRows();

        plus(box);
        // No placeholder: the empty prompt shows the plus and the caret only.
        PrKitTextArea area{};
        area.key = prUiKey(ui, kPrLayoutNodeNone, PrStringView{"prompt", 6});
        area.text = sv(model.tab(active).draft);
        area.rows = view.promptRows;
        PrLayoutNode edit = prKitTextArea(kit, box, area, PrKitFieldStyle{});
        PrKitPlace fill{};
        fill.flex = PrKitFlex_Fill;
        prKitPlace(kit, edit, &fill);
        frameless(edit);
        view.prompt = edit;
        if (prUiFocus(ui) != edit)
            prUiSetFocus(ui, edit);
        PrStringView changed{};
        if (prKitTextChanged(kit, edit, &changed))
            model.set_draft(active, std::string(changed.data, changed.size));
        if (!composing && send) {
            if (view.submit)
                view.submit(active, false);
            else
                model.send(active);
        }
        if (!composing && queue) {
            if (view.submit)
                view.submit(active, true);
            else
                model.queue(active);
        }
        // A friend's draft comes back with the caret at its end.
        if (view.prompted != model.tab(active).id) {
            view.prompted = model.tab(active).id;
            uint32_t end = uint32_t(model.tab(active).draft.size());
            prUiTextEditSetSelection(ui, edit, PrUiTextSelection{end, end});
        }
    }

    void build(PrLayoutNode parent) {
        PrLayoutNode window = element(parent, Part::Window);
        // The window's keys are claimed on the outermost node of the round: the parent the host builds under (the
        // studio window's surface, the tree's root), else the window. With no focus, as on the sprint tab, which has no
        // prompt, Prax routes a key from the tree's root up (ui.cpp routeKey), so a claim on the window, below the root,
        // would never be reached; with focus, the node is still an ancestor of the focused prompt or terminal.
        const PrLayoutNode keys = parent != kPrLayoutNodeNone ? parent : window;
        // Command-1 to Command-0 select the tabs at positions 1 to 10 in the strip's current order, in the keyboard
        // row's order. A key whose tab does not exist is refused by the model and changes nothing. macOS types no text
        // for a Command chord.
        for (std::size_t n = 0; n < kTabKeys.size(); ++n) {
            if (prUiKeyPressed(ui, keys, PrAppKey(kTabKeys[n]), PrAppModifierFlag_Super))
                model.select_tab(n);
        }
        // The text scale's keys, claimed the same way, ask for a new scale; the next frame applies it (beginWindowFrame).
        for (const TextKey& t : kTextKeys) {
            if (prUiKeyPressed(ui, keys, t.key, t.modifiers))
                view.askedTextSteps = std::clamp(view.askedTextSteps + t.steps, -kTextStepsDown, kTextStepsUp);
        }
        // Command-T adds a terminal tab at the end and shows it; Command-W closes the tab that shows if it is a
        // terminal's, and does nothing on a friend's. Claimed like the keys above: a terminal leaves
        // Command chords to the window (terminal.cpp claimKeys), and the prompt claims neither.
        if (prUiKeyPressed(ui, keys, PrAppKey('T'), PrAppModifierFlag_Super) && view.terminals != nullptr) {
            const std::size_t id = model.add_terminal();
            if (!view.terminals->open(id))
                model.close_tab(model.active());
        }
        if (prUiKeyPressed(ui, keys, PrAppKey('W'), PrAppModifierFlag_Super) && model.tab_count() != 0 &&
                model.tab(model.active()).kind == TabKind::Terminal) {
            const std::size_t id = model.tab(model.active()).id;
            model.close_tab(model.active());
            view.terminals->close(id);
        }
        serviceTerminals();
        // On a terminal tab, Escape and Command-Escape are not claimed: the terminal claims Escape for its shell, and
        // Command-Escape does nothing there. On the sprint tab Escape hides, and Command-Escape does nothing.
        const TabKind activeKind = model.tab_count() == 0 ? TabKind::Friend : model.tab(model.active()).kind;
        const bool onFriend = activeKind == TabKind::Friend;
        const bool hides = activeKind != TabKind::Terminal;
        // While an input method composes, its Enter commits the composition and its Escape cancels it (the KeyDown comes
        // first, then CompositionEnd): read before the area takes this round's events.
        const bool composing = view.prompt != kPrLayoutNodeNone && prUiTextEditIsComposing(ui, view.prompt);
        // Escape hides the overlay (view.hide; unset in --windowed, where it does nothing). It is claimed
        // only while no tab is being dragged: a claim comes before Prax's own Escape, which cancels a drag (ui.cpp
        // routeKey), so during a drag Escape cancels it and hides nothing. While composing it is the input method's.
        if (hides && prUiDragging(ui) == nullptr && prUiKeyPressed(ui, keys, PrAppKey_Escape, 0) && !composing &&
                view.hide)
            view.hide();
        // Command-Escape is stop (view.stop): nothing runs in this layer that could be stopped; a later layer connects
        // it to stopping the friend's work. It hides nothing. The text area claims neither key, so neither types or edits.
        if (onFriend && prUiKeyPressed(ui, keys, PrAppKey_Escape, PrAppModifierFlag_Super) && view.stop)
            view.stop();
        strips(window);
        // the strip may have selected or moved a tab: read the active tab again
        if (model.tab_count() != 0 && model.tab(model.active()).id != view.selectedId) {
            view.selectedId = model.tab(model.active()).id;
            if (view.selected)
                view.selected();
        }
        const bool onSprint = model.tab_count() != 0 && model.tab(model.active()).kind == TabKind::Sprint;
        if (!onSprint && view.board != nullptr)
            view.board->hidden();
        if (onSprint) {
            sprint(window);
            return;
        }
        view.boardBox = kPrLayoutNodeNone;
        if (model.tab_count() != 0 && model.tab(model.active()).kind == TabKind::Terminal) {
            terminal(window, model.tab(model.active()));
            return;
        }
        transcript(window);
        PrLayoutNode footer = element(window, Part::Footer);
        // The link's state, plainly, where the prompt stands; until the server is reached there is no prompt to type in.
        if (!view.status.empty() && model.tab_count() != 0)
            label(footer, view.status, textStyle(PrKitTextRole_BodySmall, PrKitTone_Muted, PrKitTextAlign_Center));
        if (model.tab_count() != 0 && view.promptOpen) {
            prompt(footer, composing);
        } else {
            view.prompted = ~std::size_t(0);
            view.prompt = kPrLayoutNodeNone;
            view.promptBox = kPrLayoutNodeNone;
        }
    }
};

} // namespace

PrKitTheme windowTheme(PrFontSet* fonts, bool overlay, int steps) {
    const float k = textScale(steps);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, fonts);
    // The palette is public, but which slot each widget draws with (the tab strip bg1, a hovered tab bg2, the strip's
    // rule line2) is read from the kit's source, not documented: Prax has no public per-widget colours (a request).
    PrKitPalette& p = theme.palette;
    p.bg0 = overlay ? faded(kBackground, kOverlayOpacity) : kBackground;   // the canvas
    p.bg1 = overlay ? faded(kStrip, kOverlayStripFill) : kStrip;            // the tab strip
    p.bg2 = kPromptFill;                                                    // a tab under the pointer
    p.line2 = kStripLine;                                                   // the strip's rule
    p.text1 = kInk;
    p.text3 = kNotice;
    p.text4 = kFaint;
    p.textSelection = kSelection;
    auto type = [&](PrKitTextRole role, float size, float lineHeight) {
        PrKitType& t = theme.types[role];
        t.mono = false;
        t.italic = false;
        t.size = size * k;
        t.weight = 400;
        t.lineHeight = lineHeight * k;
        t.letterSpacing = 0;
    };
    type(PrKitTextRole_Body, kBodySize, kLinePitch);                 // transcript, prompt, tabs
    type(PrKitTextRole_BodySmall, kLabelSize, 1.2f * kLabelSize);    // the notice when there are no friends
    type(PrKitTextRole_Meta, kMetaSize, 1.3f * kMetaSize);           // "queued"
    return theme;
}

bool beginWindowFrame(View& view, PrKit* kit, PrFontSet* fonts) {
    Look& look = *view.look;
    if (look.textSteps == view.askedTextSteps)
        return false;
    look.textSteps = view.askedTextSteps;
    PrKitTheme theme = windowTheme(fonts, look.overlay, look.textSteps);
    prKitSetTheme(kit, &theme);
    if (view.terminals != nullptr)
        view.terminals->setTextScale(textScale(look.textSteps));
    if (view.board != nullptr)
        view.board->setTextScale(textScale(look.textSteps));
    return true;
}

bool windowWantsFrame(const View& view) {
    return view.look->textSteps != view.askedTextSteps;
}

void buildWindow(View& view, PrKit* kit, PrLayoutNode parent) {
    Builder b{view, kit, kit->ui, kit->tree, *view.look, *view.model, {}};
    b.build(parent);
}

} // namespace nexus
