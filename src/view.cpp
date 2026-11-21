#include "view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

#include "pr/color_space.h"
#include "pr/kit_choice.h"
#include "pr/kit_button.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
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
    TranscriptAtEnd,
    Column,
    Centered,
    Entry,
    Bubble,
    CaptionBubble,
    MessageCard,
    FocusedCard,
    ActionRow,
    ReplyRow,
    Footer,
    Prompt,
    StripRow,
    BoardBox,
    Tools,
    Cursor,
    Spacer,
    Paragraphs,
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
            case Part::TranscriptAtEnd:
                box.display = PrLayoutDisplay_Flex;
                box.minHeight = px(0);
                box.overflowX = PrLayoutOverflow_Hidden;
                box.overflowY = PrLayoutOverflow_Scroll;
                // At the end, the transcript follows it (prUiFollowEnd), and the items in sight slide as entries arrive:
                // anchoring would hold the view on what showed instead, so it is off there.
                box.overflowAnchor =
                    part == Part::TranscriptAtEnd ? PrLayoutOverflowAnchor_None : PrLayoutOverflowAnchor_Auto;
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
                box.display = PrLayoutDisplay_Flex;
                box.margin[PrRectSide_Top] = prLayoutLengthAuto();
                box.margin[PrRectSide_Bottom] = prLayoutLengthAuto();
                break;
            case Part::ReplyRow:
            case Part::Entry:
                box.display = PrLayoutDisplay_Flex;
                break;
            case Part::ActionRow:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kColumnPercent);
                box.margin[PrRectSide_Left] = prLayoutLengthAuto();
                box.margin[PrRectSide_Right] = prLayoutLengthAuto();
                break;
            case Part::MessageCard:
            case Part::FocusedCard:
                box.display = PrLayoutDisplay_Flex;
                box.minWidth = px(0);
                box.maxWidth = prLayoutLengthPercent(100);
                padding(box, kBubblePadY * k, kBubblePadX * k, kBubblePadY * k, kBubblePadX * k);
                break;
            case Part::Bubble:
                box.display = PrLayoutDisplay_Block;
                box.maxWidth = prLayoutLengthPercent(kBubbleMaxPercent);
                padding(box, kBubblePadY * k, kBubblePadX * k, kBubblePadY * k, kBubblePadX * k);
                break;
            case Part::CaptionBubble:
                box.display = PrLayoutDisplay_Block;
                box.maxWidth = prLayoutLengthPercent(100);
                padding(box, kToolGap * k, kBubblePadX * k / 2, kToolGap * k, kBubblePadX * k / 2);
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
            case Part::Tools:
                box.display = PrLayoutDisplay_Flex;
                box.maxWidth = prLayoutLengthPercent(kBubbleMaxPercent);
                padding(box, kToolGap * k, kBubblePadX * k / 2, kToolGap * k, kBubblePadX * k / 2);
                box.minWidth = px(0);
                box.overflowX = PrLayoutOverflow_Hidden;
                break;
            case Part::Cursor:
                // Just under the last text: the column's gap less kCursorGap is taken back above it.
                box.display = PrLayoutDisplay_Block;
                box.width = px(kCursorWidth * k);
                box.height = px(kCursorHeight * k);
                box.margin[PrRectSide_Top] = px(-(kGap - kCursorGap) * k);
                break;
            case Part::Spacer:
                // In place of the transcript's items out of sight: its height is placed each round (prKitPlace).
                box.display = PrLayoutDisplay_Block;
                break;
            case Part::Paragraphs:
                // The open entry, a label a paragraph, so that only its last paragraph is set again as it grows.
                box.display = PrLayoutDisplay_Flex;
                box.minWidth = px(0);
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
                d.background = prColor16FromColor(look.overlay ? faded(kBackground, look.overlayOpacity) : kBackground);
                break;
            case Part::MessageCard:
            case Part::FocusedCard:
                d.background = prColor16FromColor(rgb(look.messageCard.fill));
                radius(d, look.messageCard.radius * k);
                if (part == Part::FocusedCard) {
                    d.outlineStyle = PrDrawBorderStyle_Solid;
                    d.outlineColor = prColor16FromColor(rgb(look.messageCard.ink));
                    d.outlineWidth = prLayoutUnitFromPx(2 * k);
                    d.outlineOffset = prLayoutUnitFromPx(-4 * k);
                }
                break;
            case Part::Bubble:
            case Part::CaptionBubble:
            case Part::Tools:
                d.background = prColor16FromColor(rgb(look.transcriptStyle.fill));
                radius(d, look.transcriptStyle.radius * k);
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
            case Part::TranscriptAtEnd:
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
            case Part::Centered:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.alignItems = PrLayoutAlignment_Center;
                break;
            case Part::ActionRow:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.grow = 1;
                flex.basis = px(0);
                flex.alignItems = PrLayoutAlignment_Center;
                flex.justifyContent = PrLayoutAlignment_Center;
                break;
            case Part::MessageCard:
            case Part::FocusedCard:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.shrink = 0;
                break;
            case Part::ReplyRow:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.alignItems = PrLayoutAlignment_FlexStart;
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
            case Part::Tools:
                flex.alignSelf = PrLayoutAlignment_FlexStart;
                flex.direction = PrLayoutFlexDirection_Column;
                flex.rowGap = px(kToolGap * k);
                flex.shrink = 0;
                break;
            case Part::Cursor:
                flex.alignSelf = PrLayoutAlignment_FlexStart;
                flex.shrink = 0;
                break;
            case Part::Spacer:
                flex.shrink = 0;
                break;
            case Part::Paragraphs:
                // The blank line between two paragraphs, as the one label drew it.
                flex.direction = PrLayoutFlexDirection_Column;
                flex.rowGap = px(kLinePitch * k);
                flex.shrink = 0;
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

// The state cursor's fill: its state's OKLCH colour (cursor.h) at a breath level, in sRGB for the layout. One record
// per state and level (kBreathLevels for the busy one), so the breathe makes a bounded set of records.
struct CursorRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    unsigned char state;
    unsigned char level;
    signed char steps;

    void make(const Look&, PrLayoutDecorationStyle& d) const {
        const Oklch c = cursorColor(CursorState(state), level);
        const double in[3] = {c.l, c.c, c.h};
        double out[3] = {};
        prColorConvert(PrColorSpace_Oklch, PrColorSpace_Srgb, in, out);
        auto channel = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
        d.background = prColor16FromColor(PrColor{channel(out[0]), channel(out[1]), channel(out[2]), 1});
        radius(d, kCursorRadius * textScale(steps));
    }
};

PrLayoutStyleIndex cursorStyle(PrUi* ui, const Look& look, CursorState state, int level) {
    signed char s = static_cast<signed char>(look.textSteps);
    return pr::uiStyle(ui, look, BoxRecipe{Part::Cursor, s},
        CursorRecipe{static_cast<unsigned char>(state), static_cast<unsigned char>(level), s},
        FlexRecipe{Part::Cursor, s});
}

// ---------------------------------------------------------------------------------------------------------------------
// The tree.

PrStringView sv(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

// The kit's id of the tab each strip holds but no round declares (syncTabs).
constexpr uint64_t kNoTab = ~uint64_t(0);

PrKitTextStyle textStyle(PrKitTextRole role, PrKitTone tone, PrKitTextAlign align = PrKitTextAlign_Start) {
    return PrKitTextStyle{.role = role, .tone = tone, .wrap = PrKitWrap_Auto, .align = align, .preserve = true};
}

// Dedicated theme slots keep transcript colors independent of controls and the sign-in card.
constexpr auto kChatTone = PrKitTone_AxisX;
constexpr auto kPersonTone = PrKitTone_AxisW;
constexpr auto kNoticeTone = PrKitTone_AxisY;
constexpr auto kStatusTone = PrKitTone_AxisZ;

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

    void caption(PrLayoutNode parent, std::string_view text, PrKitTextStyle appearance) {
        if (!text.empty())
            label(element(parent, Part::CaptionBubble), text, appearance);
    }

    void accountPanel(PrLayoutNode parent) {
        const auto& selected = view.windowApp.signInAccount;
        if (selected.name.empty()) return;
        const auto footer = element(parent, Part::ActionRow);
        const auto button = prKitButton(kit, footer, {.key = prUiKey(ui, footer, sv("sign-in-action"))}, {});
        const auto appearance = prUiFocus(ui) == button || prUiIsHovered(ui, button) ?
            Part::FocusedCard : Part::MessageCard;
        prLayoutTreeFrameSetStyle(tree, button, style(ui, look, appearance));
        auto large = textStyle(PrKitTextRole_Display, PrKitTone_Inverse, PrKitTextAlign_Center);
        large.wrap = PrKitWrap_Wrap;
        label(button, "Sign in to " + (selected.label.empty() ? selected.name : selected.label), large);
        if (!selected.email.empty()) {
            const auto email = label(button, selected.email, large);
            // Keep the Kit label's typography and wrapping. Its color belongs only to this address.
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, email));
            auto ink = prLayoutColorStyleRecord(tree, record.groups[PrLayoutGroup_Color]);
            ink.color = prColor16FromColor(rgb(look.messageCard.emailInk));
            record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &ink);
            prLayoutTreeFrameSetStyle(tree, email, prLayoutStyleIntern(tree, &record));
        }
        label(button, "Press Enter or click to sign in.", textStyle(PrKitTextRole_BodySmall, PrKitTone_Inverse, PrKitTextAlign_Center));
        if (prKitActivated(kit, button) && view.activateSignIn)
            view.activateSignIn();
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
    struct Drop { uint64_t id = 0; std::string group{}; std::size_t slot = 0; };
    Drop drop{};
    // The secret request the dialog shows this round, or null (read once, before the keys are claimed).
    const SecretAsk* asking = nullptr;

    // The account control fits beneath the friend title. Only the popover floats outside the strip.
    void accountDropdown(PrLayoutNode title, const std::string& current) {
        view.accountAnchor = prKitButton(kit, title, {.key = prUiKey(ui, title, sv("account"))},
                                        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
        PrKitPlace size{};
        size.height = prKitPx(kit->theme.types[PrKitTextRole_Meta].lineHeight + 4 * k());
        prKitPlace(kit, view.accountAnchor, &size);
        auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, view.accountAnchor));
        auto& group = record.groups[PrLayoutGroup_Decoration];
        auto decoration = prLayoutDecorationStyleRecord(tree, group);
        decoration.background = prColor16FromColor(rgb(look.transcriptStyle.fill));
        group = prLayoutDecorationStyleIntern(tree, &decoration);
        prLayoutTreeFrameSetStyle(tree, view.accountAnchor, prLayoutStyleIntern(tree, &record));
        label(view.accountAnchor, current, textStyle(PrKitTextRole_Meta, kStatusTone));
        prKitIcon(kit, view.accountAnchor, {.icon = PrKitIconKind_ChevronDown},
                  {.size = kMetaSize * k() * 0.7f, .tone = kStatusTone, ._pad = {}});
        if (!asking && prKitActivated(kit, view.accountAnchor) && view.pickerCommand)
            view.pickerCommand(view.accountPicker ? "cancel" : "open");

        const bool open = !asking && view.accountPicker && view.accountPicker->tab == model.tab(model.active()).id;
        prKitSetOpen(kit, view.accountAnchor, open);
        const auto popover = prKitPopover(kit, view.accountAnchor, {}, {});
        if (!open || popover == kPrLayoutNodeNone) {
            view.lastHighlightedAccount.clear();
            if (open && view.pickerCommand) view.pickerCommand("cancel");
            return;
        }
        // The fork handles arrow keys and Enter. Native focus follows that preview and remains available to mouse input.
        view.accountMenu = prKitMenu(kit, popover, {}, {.leavesFocus = true});
        captureShortcuts(view.accountMenu);
        if (!view.accountPicker) return;
        const auto& picker = *view.accountPicker;
        std::string chosen;
        for (std::size_t i = 0; i < picker.options.size(); ++i) {
            const auto& option = picker.options[i];
            const auto& displayed = option.label.empty() ? option.name : option.label;
            const bool preview = i == picker.highlighted;
            const auto row = prKitMenuItem(kit, view.accountMenu,
                {.key = prUiKey(ui, view.accountMenu, sv(option.name)), .text = sv(displayed),
                 .icon = PrKitIconKind(preview ? PrKitIconKind_ChevronRight : PrKitIconKind_None),
                 .checked = option.name == picker.current}, {});
            PrKitPlace rowSize{};
            rowSize.height = prKitPx(kit->theme.types[PrKitTextRole_Body].lineHeight + 8 * k());
            prKitPlace(kit, row, &rowSize);
            view.accountRows.push_back(row);
            if (preview && view.lastHighlightedAccount != option.name) {
                prUiSetFocus(ui, row);
                prUiReveal(ui, row);
                view.lastHighlightedAccount = option.name;
            }
            if (prKitActivated(kit, row)) chosen = option.name;
        }
        if (!chosen.empty() && view.chooseAccount) view.chooseAccount(chosen);
    }

    void strip(PrLayoutNode parent, PrKitTabs* tabs, std::size_t first, std::size_t end, const std::string& group,
            bool exchanges, PrKitFlex flex) {
        syncTabs(tabs, first, end);
        PrLayoutNode row = prKitTabStrip(kit, parent, PrKitTabStrip{.key = prUiKey(ui, parent, sv(group)), .tabs = tabs,
            .group = exchanges ? 0x4e78546162477270ull : 0, .reorderable = true, .collapsesSingle = false, .disabled = false},
            PrKitChoiceStyle{.size = PrKitControlSize_Lg, .direction = PrKitDirection_Row});
        PrKitPlace rowPlace{};
        rowPlace.height = prKitPx(kStripHeight * k() + 1);
        rowPlace.flex = flex;
        prKitPlace(kit, row, &rowPlace);
        PrKitTabMove moved{};
        if (prKitTabMoved(kit, row, &moved) && moved.kind == PrKitTabMoveKind_Received && moved.id == view.draggingTab)
            drop = Drop{moved.id, group, moved.index};
        mirrorMove(tabs, first, end);
        const uint64_t selected = prKitTabsSelected(tabs);
        for (std::size_t i = first; i < end; ++i) {
            PrLayoutNode tab = prKitTab(kit, row, PrKitTab{.id = kitId(model.tab(i)), .disabled = false});
            // The strip leaves empty space translucent. The title sits on an opaque tab; Prax's hover
            // fill, chosen underline and selected text retain their normal appearance.
            const auto base = prLayoutTreeFrameStyle(tree, tab);
            auto record = prLayoutStyleRecord(tree, base);
            auto& group = record.groups[PrLayoutGroup_Decoration];
            auto decoration = prLayoutDecorationStyleRecord(tree, group);
            const PrColor fill = prColorFromColor16(decoration.background);
            if (fill.a < 1) {
                const float uncovered = 1 - fill.a;
                decoration.background = prColor16FromColor({fill.r * fill.a + kStrip.r * uncovered,
                    fill.g * fill.a + kStrip.g * uncovered, fill.b * fill.a + kStrip.b * uncovered, 1});
                group = prLayoutDecorationStyleIntern(tree, &decoration);
                prLayoutTreeFrameSetStyle(tree, tab, prLayoutStyleIntern(tree, &record));
            }
            PrKitPlace tabPlace{};
            const bool currentFriend = i == model.active() && model.tab(i).kind == TabKind::Friend;
            const bool offersAccounts = std::find(view.windowApp.panels.begin(), view.windowApp.panels.end(),
                                                  Panel::Account) != view.windowApp.panels.end();
            const auto current = currentFriend && offersAccounts && view.accountCaption ? view.accountCaption() : std::string();
            tabPlace.height = prKitPx((current.empty() ? 1.75f * kTabSize : kStripHeight) * k());
            tabPlace.align = PrKitAlign_Center;
            if (i == 0)
                tabPlace.margin.left = 0.8f * kU * k();
            prKitPlace(kit, tab, &tabPlace);
            if (current.empty()) {
                prKitTabLabel(kit, tab, {.text = sv(label(model.tab(i)))});
            } else {
                const auto title = prKitBox(kit, tab, {}, {.direction = PrKitDirection_Column, .align = PrKitAlign_Start});
                prKitTabLabel(kit, title, {.text = sv(label(model.tab(i)))});
                accountDropdown(title, current);
            }
            view.tabNodes[i] = tab;
        }
        // A click on a tab, or Left or Right on a focused one, selects in the kit: made through the model's select_tab.
        // Right on the last tab selects kNoTab, which selects nothing: the next round puts the selection back.
        const uint64_t now = prKitTabsSelected(tabs);
        if (now != selected && now != kNoTab)
            model.select_tab(first + prKitTabsIndex(tabs, now));
    }

    // Prax keeps click selection independent of drag admission. Every contiguous group has its own strip. A fork
    // that permits transfers uses the kit's cross-strip drop event, naming the source instance and destination slot.
    void strips(PrLayoutNode window) {
        view.tabNodes.assign(model.tab_count(), kPrLayoutNodeNone);
        view.accountAnchor = kPrLayoutNodeNone;
        view.accountMenu = kPrLayoutNodeNone;
        view.accountRows.clear();
        const bool exchanges = std::any_of(view.windowApp.groups.begin(), view.windowApp.groups.end(),
            [](const TabGroup& group) { return !group.acceptsFrom.empty(); });
        if (const PrUiPayload* dragging = prUiDragging(ui)) view.draggingTab = dragging->value;
        const bool many = model.tab_count() && model.tab(0).group != model.tab(model.tab_count() - 1).group;
        const PrLayoutNode row = many ? element(window, Part::StripRow) : window;
        for (std::size_t first = 0; first < model.tab_count();) {
            const auto& group = model.tab(first).group;
            auto end = first + 1;
            bool extensions = model.tab(first).kind == TabKind::Extension;
            while (end < model.tab_count() && model.tab(end).group == group) {
                extensions = extensions && model.tab(end).kind == TabKind::Extension;
                ++end;
            }
            auto& tabs = view.groupStrips[group];
            if (!tabs) tabs.reset(prKitTabsCreate());
            if (many && extensions) caption(row, group, textStyle(PrKitTextRole_BodySmall, kStatusTone));
            strip(row, tabs.get(), first, end, group, exchanges,
                  many && end == model.tab_count() ? PrKitFlex_Fill : PrKitFlex_Fixed);
            first = end;
        }
        if (drop.id) {
            for (std::size_t i = 0; i < model.tab_count(); ++i) {
                if (kitId(model.tab(i)) == drop.id && model.move_to_group(i, drop.group, drop.slot)) {
                    if (view.moved) view.moved();
                    break;
                }
            }
        }
        if (!prUiDragging(ui)) view.draggingTab = 0;
        keepFocusOnTheActiveTab();
    }

    // A press focuses the tab it lands on (ui.cpp PointerDown), a drag included, and the sprint tab has no prompt to
    // take the focus back: left there, the focus ring stood on the dragged tab once a key made the focus visible (an
    // Escape that hid the overlay), and Enter or Space reaching the tree selected it. So the keyboard focus never rests
    // on a tab other than the active one: it goes to the active tab, and the prompt or a terminal takes it from there
    // later in the round, as each does every round.
    void keepFocusOnTheActiveTab() {
        const std::size_t active = model.active();
        for (std::size_t i = 0; i < view.tabNodes.size(); ++i) {
            if (i != active && view.tabNodes[i] != kPrLayoutNodeNone && prUiFocus(ui) == view.tabNodes[i] &&
                    active < view.tabNodes.size() && view.tabNodes[active] != kPrLayoutNodeNone)
                prUiSetFocus(ui, view.tabNodes[active]);
        }
    }

    // A friend's tab shows the friend's name; the sprint tab "sprint"; a terminal's its label (Terminals::title: the
    // machine its ssh reaches, or the title its shell set), or "terminal".
    std::string label(const Tab& tab) {
        if (tab.kind == TabKind::Friend || tab.kind == TabKind::Extension)
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
        HitchScope scope(view.hitch, Phase::TerminalTab, std::uint32_t(tab.id));
        // A shell that cannot run here at all (on Windows, no WSL2): the line that says what is missing, centred as the
        // sprint tab's is.
        const std::string problem = view.terminals->problem(tab.id);
        if (!problem.empty()) {
            PrLayoutNode box = element(window, Part::BoardBox);
            PrLayoutNode column = element(box, Part::Column);
            PrLayoutNode centered = element(column, Part::Centered);
            caption(centered, problem, textStyle(PrKitTextRole_Body, kNoticeTone, PrKitTextAlign_Center));
            return;
        }
        view.terminals->show(tab.id, window);
    }

    // The sprint tab: its page fills the window's width and the height below the strip, with no prompt; or, while there
    // is no page to show, the line that says why. With no friends, a caption beneath the board gives the reason.
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
            caption(centered, problem, textStyle(PrKitTextRole_Body, kNoticeTone, PrKitTextAlign_Center));
        }
        bool friends = false;
        for (std::size_t i = 0; i < model.tab_count() && !friends; ++i)
            friends = model.tab(i).kind == TabKind::Friend;
        if (!friends && !view.notice.empty()) {
            PrLayoutNode footer = element(window, Part::Footer);
            caption(footer, view.notice, textStyle(PrKitTextRole_BodySmall, kStatusTone, PrKitTextAlign_Center));
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
            HitchScope scope(view.hitch, Phase::TerminalTab, std::uint32_t(id));
            if (view.terminals->ended(id)) {
                model.close_tab(i);
                view.terminals->close(id);
            } else if (i != model.active()) {
                view.terminals->update(id);
            }
        }
    }

    // The friend's transcript as laid out (TranscriptItems), with the entries that arrived since grouped into items:
    // an entry each, and a run of tool lines one item.
    TranscriptItems& itemsOf(const Tab& tab) {
        if (view.items.size() <= tab.id)
            view.items.resize(tab.id + 1);
        TranscriptItems& items = view.items[tab.id];
        const std::vector<Entry>& entries = tab.transcript;
        for (std::size_t i = items.entries; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if ((e.kind == EntryKind::Tool && !look.transcriptStyle.showTools) ||
                    (e.kind == EntryKind::Notice && !look.transcriptStyle.showNotices))
                continue;
            uint32_t lines = 1;
            for (char c : e.text)
                lines += c == '\n' ? 1 : 0;
            const bool joins = e.kind == EntryKind::Tool && i != 0 && entries[i - 1].kind == EntryKind::Tool &&
                !items.first.empty();
            if (joins) {
                items.lines.back() += 1;
                items.chars.back() += uint32_t(e.text.size());
                items.count.back() += 1;
                items.height.back() = 0;    // it grew: laid out again before its height is known
                continue;
            }
            items.first.push_back(uint32_t(i));
            items.lines.push_back(lines);
            items.chars.push_back(uint32_t(e.text.size()));
            items.count.push_back(1);
            items.height.push_back(0);
        }
        items.entries = entries.size();
        return items;
    }

    // The heights of the items the last round declared, from the last layout, with the gap after each; and the
    // column's width. After a change of the text scale every height is laid out again.
    void measure(TranscriptItems& items, bool switched) {
        if (items.steps != look.textSteps) {
            std::fill(items.height.begin(), items.height.end(), 0.0f);
            items.steps = look.textSteps;
        }
        if (!switched && items.column != kPrLayoutNodeNone) {
            PrRect c = prLayoutRectToPx(prLayoutNodeBounds(tree, items.column, PrLayoutCoordinates_Layout));
            if (c.right - c.left >= 1)
                items.width = c.right - c.left;
            for (const auto& [item, node] : items.declared) {
                PrRect r = prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Layout));
                if (item < items.height.size() && r.bottom - r.top > 0)
                    items.height[item] = r.bottom - r.top + kGap * k();
            }
        }
        items.declared.clear();
        if (items.width < 1)
            items.width = view.extent.x * kColumnPercent / 100;
    }

    // An item's height with the gap after it: as last laid out, or an estimate from its text until it is.
    float heightOf(const TranscriptItems& items, std::size_t i, const Tab& tab) const {
        if (items.height[i] > 0)
            return items.height[i];
        const float s = k();
        const float gap = kGap * s;
        const EntryKind kind = tab.transcript[items.first[i]].kind;
        if (kind == EntryKind::Tool)
            return float(items.count[i]) * kToolPitch * s + float(items.count[i] + 1) * kToolGap * s + gap;
        // Characters of the body face are about half its size wide.
        const float perChar = 0.5f * kBodySize * s;
        const float width = std::max(items.width * kBubbleMaxPercent / 100 - 2 * kBubblePadX * s, 1.0f);
        const float pad = 2 * kBubblePadY * s;
        const float rows = float(items.lines[i]) + std::floor(float(items.chars[i]) * perChar / width);
        return rows * kLinePitch * s + pad + gap;
    }

    // Declares the items from the scroll's `offset` (or the end) a screen either side of the `viewport`, the spacers
    // for the items before and after them, and keeps which items it declared for the next round to measure.
    void declareItems(PrLayoutNode scroller, PrLayoutNode column, const Tab& tab, TranscriptItems& items, bool atEnd,
            float offset, float viewport) {
        const std::size_t n = items.first.size();
        if (n == 0)
            return;
        std::size_t first = 0;
        std::size_t end = n;
        float above = 0;        // the items before `first`, px
        float below = 0;        // the items from `end` on, px
        if (atEnd) {
            // From the newest back, two screens' worth.
            float acc = 0;
            first = n;
            while (first > 0 && acc < 2 * viewport) {
                --first;
                acc += heightOf(items, first, tab);
            }
            for (std::size_t i = 0; i < first; ++i)
                above += heightOf(items, i, tab);
        } else {
            const float top = offset - viewport;
            const float bottom = offset + 2 * viewport;
            float y = kColumnInset * k();
            first = n;
            end = n;
            for (std::size_t i = 0; i < n; ++i) {
                const float h = heightOf(items, i, tab);
                if (first == n && y + h > top) {
                    first = i;
                }
                if (first != n && y > bottom) {
                    end = i;
                    break;
                }
                if (first == n) {
                    above += h;
                }
                y += h;
            }
            // No item reaches the top (the offset is past the content, as when it shrank): the last item is declared,
            // and the spacer above stands for the items before it only.
            if (first == n) {
                first = n - 1;
                above -= heightOf(items, first, tab);
            }
            for (std::size_t i = end; i < n; ++i)
                below += heightOf(items, i, tab);
        }
        // At the end, the items in sight slide as entries arrive, and the spacer above grows by what left them: the
        // transcript follows its end there with scroll anchoring off (Part::TranscriptAtEnd), which would hold the view
        // on what showed instead.
        if (atEnd)
            prLayoutTreeFrameSetStyle(tree, scroller, style(ui, look, Part::TranscriptAtEnd));
        const uint64_t base = uint64_t(tab.id) << 8;
        // A spacer is followed by the column's gap, as an item is: its own height leaves that out.
        if (first != 0)
            spacer(column, prUiKeyIndex2(ui, kPrLayoutNodeNone, base | 1, 0), above - kGap * k());
        for (std::size_t i = first; i < end; ++i)
            items.declared.emplace_back(uint32_t(i), declareItem(column, tab, items, i, base));
        if (end != n)
            spacer(column, prUiKeyIndex2(ui, kPrLayoutNodeNone, base | 1, 1), below - kGap * k());
    }

    void spacer(PrLayoutNode column, PrUiKey key, float height) {
        PrLayoutNode node = element(column, Part::Spacer, 0, key);
        PrKitPlace place{};
        place.height = prKitPx(std::max(height, 0.0f));
        prKitPlace(kit, node, &place);
    }

    // One item, keyed by its tab and place, so that it keeps its node and its set text as the items in sight change.
    PrLayoutNode declareItem(PrLayoutNode column, const Tab& tab, const TranscriptItems& items, std::size_t i,
            uint64_t base) {
        const PrUiKey key = prUiKeyIndex2(ui, kPrLayoutNodeNone, base | 2, items.first[i]);
        const Entry& e = tab.transcript[items.first[i]];
        switch (e.kind) {
            case EntryKind::Person:
            case EntryKind::Friend:
            case EntryKind::Notice: {
                PrLayoutNode entry = element(column, e.kind == EntryKind::Person ? Part::Entry : Part::ReplyRow, 0, key);
                PrLayoutNode bubble = element(entry, Part::Bubble);
                const auto conversationTone = e.kind == EntryKind::Person ? kPersonTone : kChatTone;
                label(bubble, e.text, textStyle(PrKitTextRole_Body, e.kind == EntryKind::Notice ? kNoticeTone : conversationTone));
                return entry;
            }
            case EntryKind::Tool:
                break;
        }
        PrLayoutNode tools = element(column, Part::Tools, 0, key);
        PrKitTextStyle line = textStyle(PrKitTextRole_Mono, kStatusTone);
        line.wrap = PrKitWrap_Clip;
        for (std::uint32_t t = 0; t < items.count[i]; ++t)
            label(tools, tab.transcript[items.first[i] + t].text, line);
        return tools;
    }

    // The open entry as it grows, a label a paragraph (paragraphs end at a blank line): each paragraph but the last
    // keeps its text, so only the last is set again as text arrives.
    void paragraphs(PrLayoutNode column, const Tab& tab, const std::string& text) {
        const PrUiKey key = prUiKeyIndex2(ui, kPrLayoutNodeNone, (uint64_t(tab.id) << 8) | 3, 0);
        PrLayoutNode row = element(column, Part::ReplyRow, 0, key);
        PrLayoutNode box = element(element(row, Part::Bubble), Part::Paragraphs);
        const PrKitTextStyle body = textStyle(PrKitTextRole_Body, kChatTone);
        std::size_t at = 0;
        while (at <= text.size()) {
            std::size_t blank = text.find("\n\n", at);
            if (blank == std::string::npos) {
                label(box, std::string_view(text).substr(at), body);
                break;
            }
            label(box, std::string_view(text).substr(at, blank - at), body);
            at = blank + 2;
        }
    }

    void transcript(PrLayoutNode window) {
        PrLayoutNode scroller = element(window, Part::Transcript, PrUiFlag_Scrollable);
        PrLayoutNode column = element(scroller, Part::Column);
        if (model.tab_count() == 0) {
            PrLayoutNode centered = element(column, Part::Centered);
            caption(centered, view.notice, textStyle(PrKitTextRole_Body, kStatusTone, PrKitTextAlign_Center));
            view.transcript = scroller;
            return;
        }
        const Tab& tab = model.tab(model.active());
        const bool switched = view.shown != tab.id;
        // Text was just cut to fit this tab's prompt: the transcript goes to its end, where the line saying so shows.
        const bool toCut = view.cutFollow && view.cutTab == tab.id;
        if (keepScroll(scroller, tab.id))
            prUiFollowEnd(ui, scroller);
        if (toCut) {
            // Past the end, which layout clamps to the end of the content this round makes (as keepScroll goes there).
            prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), PrLayoutUnit{INT32_MAX}});
            view.cutFollow = false;
        }
        view.transcript = scroller;
        // The kit's labels and text area wrap, breaking inside a word that fits no line (PrKitWrap_Wrap, kit_theme.h
        // 99), so a long URL or token wraps inside its box instead of running past it. A tool line never wraps: it is
        // one compact line, cut at the column's edge, and tool lines in a row stand together, close.
        // Only the items in sight are declared, with a screen's height either side, and spacers for the rest
        // (TranscriptItems): a round's cost does not grow with the transcript.
        TranscriptItems& items = itemsOf(tab);
        measure(items, switched);
        items.column = column;
        const float viewport = std::max(view.extent.y, 1.0f);
        float offset = 0;
        bool atEnd = true;
        if (switched) {
            atEnd = tab.id >= view.scrolls.size() || view.scrolls[tab.id] == View::kAtEnd;
            offset = atEnd ? 0 : view.scrolls[tab.id];
        } else {
            PrLayoutScrollInfo info{};
            if (prLayoutNodeScrollInfo(tree, scroller, &info) && info.scrollContainer) {
                offset = prLayoutUnitToPx(info.offset.y);
                atEnd = info.offset.y.raw >= info.range.bottom.raw;
            }
        }
        atEnd = atEnd || toCut;
        declareItems(scroller, column, tab, items, atEnd, offset, viewport);
        // The friend's text still growing (the open entry), after the transcript, a label a paragraph: only its last
        // paragraph is set again as it grows. It joins the transcript, closed, when its block ends.
        const std::string* growing = view.growing ? view.growing(tab.id) : nullptr;
        if (growing != nullptr && !growing->empty()) {
            paragraphs(column, tab, *growing);
        }
        // Keep the reason alongside the conversation if a prompt could not be sent.
        if (!view.problem.empty()) {
            caption(element(column, Part::ReplyRow), view.problem, textStyle(PrKitTextRole_Body, kNoticeTone));
        }
        // How much text was cut to fit the prompt, on the tab it was cut on, there too: the transcript went to its end
        // to show it, so it stands just above the prompt.
        if (!view.cut.empty() && view.cutTab == tab.id) {
            caption(element(column, Part::ReplyRow), view.cut, textStyle(PrKitTextRole_Body, kNoticeTone));
        }
        // The state cursor, on its own line under the last text: the busy one breathes, so frames go on while it shows.
        const CursorState state = view.cursor ? view.cursor(tab.id) : CursorState::None;
        view.breathing = state == CursorState::Busy;
        if (state != CursorState::None) {
            const int level = state == CursorState::Busy ? breathLevel(view.time) : 0;
            prUiElement(ui, column, 0, cursorStyle(ui, look, state, level), 0);
        }
        // The tab's queue, after its transcript and apart from it: each prompt held for later, oldest first, in a
        // bubble marked "queued".
        for (const std::string& text : tab.queue) {
            PrLayoutNode entry = element(column, Part::Entry);
            PrLayoutNode bubble = element(entry, Part::Bubble);
            label(bubble, text, textStyle(PrKitTextRole_Body, kPersonTone));
            PrLayoutNode tag = label(bubble, "queued", textStyle(PrKitTextRole_Meta, kStatusTone, PrKitTextAlign_End));
            PrKitPlace place{};
            place.margin.top = 0.3f * kMetaSize * k();
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

    void promptKey(std::size_t active, const char* key) {
        const Tab& tab = model.tab(active);
        const bool logout = tab.draft == "/logout";
        if (!logout && view.activateSignIn && view.activateSignIn())
            return; // Keep the exact draft, even if the fork rewrites or drops empty prompts.
        policy::MessagePlan plan;
        std::string why;
        const policy::MessageState state{tab.draft, tab.name, tab.model, tab.queue.size(),
            view.cursor && view.cursor(tab.id) == CursorState::Busy};
        if (!policy::messagePlan(*view.application, {}, true, key, state, plan, why) ||
                (plan.text && plan.text->size() > kPromptBytes)) {
            view.problem = "Prompt application: " + (why.empty() ? "draft exceeds the prompt limit" : why);
            return;
        }
        if (plan.delivery == policy::Delivery::SignOut) {
            if (!view.signOut)
                view.problem = "Connect to the Nexus server to sign out.";
            else if (view.signOut())
                model.set_draft(active, {});
            return;
        }
        if (logout && (plan.delivery != policy::Delivery::None || plan.text)) {
            // A fork using the older send/queue policy must never turn the command into a message.
            view.problem = "Enable the sign_out operation in the client prompt policy to use /logout.";
            return;
        }
        if (plan.text)
            model.set_draft(active, std::move(*plan.text));
        if (plan.delivery != policy::Delivery::None) {
            const bool queue = plan.delivery == policy::Delivery::Queue;
            if (view.submit)
                view.submit(active, queue);
            else if (queue)
                model.queue(active);
            else
                model.send(active);
        }
        if (!plan.notices.empty()) {
            view.problem.clear();
            for (const auto& notice : plan.notices) {
                if (!view.problem.empty())
                    view.problem += "\n";
                view.problem += notice;
            }
        }
    }

    // `composing`: an input method was composing in the prompt before this round's events (read in build).
    void prompt(PrLayoutNode footer, bool composing) {
        std::size_t active = model.active();
        // The prompt's rounded box, holding the plus and the text area: the kit has no rounded text area and no slot
        // for an icon inside one (frameless). Enter sends and Command-Enter queues, captured on the box, so the area
        // never sees them; every other Enter (Shift-Enter among them) goes to the area, which breaks the line (pr/ui.h).
        PrLayoutNode box = element(footer, Part::Prompt);
        view.promptBox = box;
        bool send = !pickerInput && prUiKeyCaptured(ui, box, PrAppKey_Enter, 0);
        bool queue = !pickerInput && prUiKeyCaptured(ui, box, PrAppKey_Enter, style::kCommand);
        // Command-V reads the clipboard through the host, which cuts the text to the room left before the area sees it,
        // and hands it on in one event.
        if (!pickerInput && view.paste && prUiKeyCaptured(ui, box, PrAppKey('V'), style::kCommand)) {
            view.paste();
        }
        view.promptRows = promptRows();

        plus(box);
        // No placeholder: the empty prompt shows the plus and the caret only.
        PrKitTextArea area{};
        area.key = prUiKey(ui, kPrLayoutNodeNone, PrStringView{"prompt", 6});
        area.text = sv(model.tab(active).draft);
        area.rows = view.promptRows;
        area.disabled = pickerInput;
        PrLayoutNode edit = prKitTextArea(kit, box, area, PrKitFieldStyle{});
        PrKitPlace fill{};
        fill.flex = PrKitFlex_Fill;
        prKitPlace(kit, edit, &fill);
        frameless(edit);
        view.prompt = edit;
        // The focus is the secret dialog's while it shows (secretDialog).
        if (!pickerInput && asking == nullptr && prUiFocus(ui) != edit)
            prUiSetFocus(ui, edit);
        PrStringView changed{};
        const bool coming = view.cutComing;
        view.cutComing = false;     // the cut text, pushed for this frame, is in the area's events of its first round
        if (!pickerInput && prKitTextChanged(kit, edit, &changed)) {
            // Text that came past the host's cut (pasted with no host, or typed into a selection the host had not yet
            // seen) is cut here.
            const PrStringView kept = cutText(changed, kPromptBytes);
            model.set_draft(active, std::string(kept.data, kept.size));
            if (kept.size != changed.size) {
                promptCut(view, changed.size, kept.size, false);
            }
        }
        // The line about cut text stands for the draft as the cut left it, and goes once that draft changes.
        if (!view.cut.empty() && view.cutTab == model.tab(active).id) {
            if (coming) {
                view.cutDraft = model.tab(active).draft;
            } else if (view.cutDraft != model.tab(active).draft) {
                view.cut.clear();
                view.cutTab = ~std::size_t(0);
            }
        }
        if (!pickerInput && !composing && asking == nullptr) {
            if (send)
                promptKey(active, "Enter");
            if (queue)
                promptKey(active, "Command+Enter");
        }
        // A friend's draft comes back with the caret at its end.
        if (view.prompted != model.tab(active).id) {
            view.prompted = model.tab(active).id;
            uint32_t end = uint32_t(model.tab(active).draft.size());
            prUiTextEditSetSelection(ui, edit, PrUiTextSelection{end, end});
        }
    }

    // The secret dialog (docs/secrets.md), over whatever tab shows: Prax's dialog (a modal card with a title and a
    // close button), naming the account and what is asked, who asks, and the asker's reason as quoted text; then one
    // masked field and nothing else, with no placeholder. Enter sends the value; Escape, the close button or a press
    // outside the card cancels the request. The value is never in the view: the field shows what Secrets::typed() gives
    // each round, masked by Prax, and each edit goes back through Secrets::type(). Nothing here waits on the network or
    // the disk: an answer is one action handed to the link thread (frame_host.h LinkSecrets).
    void secretDialog(PrLayoutNode window) {
        if (asking == nullptr) {
            if (view.secretShown != 0) {
                prKitSetOpen(kit, window, false);
                view.secretShown = 0;
            }
            view.secretField = kPrLayoutNodeNone;
            return;
        }
        const bool fresh = view.secretShown != asking->id;
        if (fresh) {
            prKitSetOpen(kit, window, true);
            view.secretShown = asking->id;
        }
        PrKitDialog dialog{};
        dialog.key = prUiKey(ui, kPrLayoutNodeNone, sv("secret"));
        const std::string title = asking->account + ": " + asking->what;
        dialog.title = sv(title);
        const PrLayoutNode body = prKitDialog(kit, window, dialog, PrKitDialogStyle{});
        if (body == kPrLayoutNodeNone) {
            // The person closed it (Escape, the close button, a press outside): the request is cancelled.
            view.secretField = kPrLayoutNodeNone;
            if (!fresh) {
                view.secrets->cancel();
            }
            return;
        }
        label(body, asking->asker.empty() ? std::string("asked by nexus") : "asked by " + asking->asker,
              textStyle(PrKitTextRole_BodySmall, PrKitTone_Muted));
        if (!asking->why.empty()) {
            label(body, "“" + asking->why + "”", textStyle(PrKitTextRole_Body, PrKitTone_Primary));
        }
        const bool enter = prUiKeyCaptured(ui, body, PrAppKey_Enter, 0);
        PrKitTextField field{};
        field.key = prUiKey(ui, kPrLayoutNodeNone, sv("secret-field"));
        const std::string_view typed = view.secrets->typed();
        field.text = sv(typed);
        field.inputKind = PrAppTextInputKind_Password;
        const PrLayoutNode edit = prKitTextField(kit, body, field, PrKitFieldStyle{});
        view.secretField = edit;
        if (prUiFocus(ui) != edit) {
            prUiSetFocus(ui, edit);
        }
        PrStringView changed{};
        if (prKitTextChanged(kit, edit, &changed)) {
            view.secrets->type(std::string_view(changed.data, changed.size));
        }
        if (enter) {
            view.secrets->submit();
        }
    }

    // Both the window root and the detached account menu surface use these same policy bindings.
    void captureShortcuts(PrLayoutNode keys) {
        std::vector<Shortcut> pressed;
        for (const auto& binding : view.windowApp.shortcuts) {
            if ((!view.secrets || !view.secrets->showing()) && prUiKeyCaptured(ui, keys, binding.key, binding.modifiers))
                pressed.push_back(binding);
        }
        for (std::size_t i = 0; i < pressed.size(); ++i) {
            // A preceding key can select a different group in the same frame.
            if (i) updateApplication();
            const auto& bindings = view.windowApp.shortcuts;
            const auto active = std::find_if(bindings.begin(), bindings.end(), [&](const Shortcut& key) {
                return key.key == pressed[i].key && key.modifiers == pressed[i].modifiers;
            });
            if (active != bindings.end()) shortcut(*active);
        }
    }

    bool pickerInput = false;

    void build(PrLayoutNode parent) {
        PrLayoutNode window = element(parent, Part::Window);
        view.breathing = false;     // until a friend's transcript draws a busy cursor this round
        // The window's keys are claimed on the outermost node of the round: the parent the host builds under (the
        // studio window's surface, the tree's root), else the window. With no focus, as on the sprint tab, which has no
        // prompt, Prax routes a key from the tree's root up (ui.cpp routeKey), so a claim on the window, below the root,
        // would never be reached; with focus, the node is still an ancestor of the focused prompt or terminal.
        const PrLayoutNode keys = parent != kPrLayoutNodeNone ? parent : window;
        // nexus's Command chords are captured, not claimed: a capture routes ahead of every claim (pr/ui.h
        // prUiKeyCaptured), so they win in every tab, a terminal's included. That matters off macOS, where Command is
        // Alt and Prax's terminal claims every Alt chord for its shell (terminal.cpp claimKeys), which keeps Alt as
        // Meta for every other key. Inside the overlay nexus takes its keys (AGENTS.md).
        updateApplication();
        pickerInput = view.accountPicker.has_value();
        captureShortcuts(keys);
        pickerInput = pickerInput || view.accountPicker.has_value();
        serviceTerminals();
        // On a terminal tab Escape is not claimed: the terminal claims it for its shell. Command-Escape does nothing
        // there or on the sprint tab (it is captured below all the same). On the sprint tab Escape hides.
        const TabKind activeKind = model.tab_count() == 0 ? TabKind::Friend : model.tab(model.active()).kind;
        const bool hides = activeKind != TabKind::Terminal;
        // While an input method composes, its Enter commits the composition and its Escape cancels it (the KeyDown comes
        // first, then CompositionEnd): read before the area takes this round's events.
        const bool composing = view.prompt != kPrLayoutNodeNone && prUiTextEditIsComposing(ui, view.prompt);
        // Escape hides the overlay (view.hide; unset in --windowed, where it does nothing). It is claimed
        // only while no tab is being dragged: a claim comes before Prax's own Escape, which cancels a drag (ui.cpp
        // routeKey), so during a drag Escape cancels it and hides nothing. While composing it is the input method's.
        // While the secret dialog shows, Escape is the dialog's (it cancels the request) and hides nothing.
        asking = view.secrets != nullptr ? view.secrets->showing() : nullptr;
        if (hides && !pickerInput && asking == nullptr && prUiDragging(ui) == nullptr &&
                prUiKeyPressed(ui, keys, PrAppKey_Escape, 0) && !composing && view.hide)
            view.hide();
        strips(window);
        // A click on the sprint tab, selected already or not, takes its page back to the board (Board::home).
        if (view.board && model.tab_count() && model.tab(model.active()).kind == TabKind::Sprint &&
                prUiClicked(ui, view.tabNodes[model.active()], 0))
            view.board->home();
        // the strip may have selected or moved a tab: read the active tab again
        if (model.tab_count() != 0 && model.tab(model.active()).id != view.selectedId) {
            view.selectedId = model.tab(model.active()).id;
            if (view.selected)
                view.selected();
        }
        // Cut text on its way to a prompt this round does not build has nowhere to go (the focus left the prompt, and
        // the area does not see the event): nothing more is coming.
        if (model.tab_count() == 0 || model.tab(model.active()).kind != TabKind::Friend || !view.promptOpen) {
            view.cutComing = false;
        }
        page(window, composing);
        secretDialog(window);
    }

    void updateApplication() {
        const auto describe = [&] {
            const Tab* active = model.tab_count() ? &model.tab(model.active()) : nullptr;
            const char* kind = !active ? "empty" : active->kind == TabKind::Friend ? "friend" :
                active->kind == TabKind::Terminal ? "terminal" : active->kind == TabKind::Sprint ? "sprint" : "extension";
            if (view.accountPicker && (!active || active->id != view.accountPicker->tab ||
                    (view.secrets && view.secrets->showing())))
                view.accountPicker.reset();
            view.windowApp.update(*view.application, kind, active ? active->type : std::string(), view.promptOpen,
                !view.status.empty(), model.active(), model.tab_count(), active ? active->group : std::string(),
                active && active->kind == TabKind::Friend && view.signInAccount ? view.signInAccount() : SignInAccount{},
                view.accountPicker.has_value());
        };
        describe();
        if (view.tabGroupGeneration != view.windowApp.layoutGeneration) {
            model.set_tab_groups(view.windowApp.groups);
            view.tabGroupGeneration = view.windowApp.layoutGeneration;
            if (view.restoreTabLayout) view.restoreTabLayout();
            describe();
        }
    }

    void shortcut(const Shortcut& key) {
        switch (key.command) {
            case WindowCommand::SelectTab: {
                const auto index = static_cast<std::size_t>(key.argument);
                if (model.select_tab(index) && model.tab(index).kind == TabKind::Sprint && view.board)
                    view.board->home();
                break;
            }
            case WindowCommand::MoveTab: {
                const auto from = model.active();
                if (model.tab_count() == 0 || (key.argument < 0 && from == 0)) break;
                const auto to = key.argument < 0 ? from - 1 : from + 1;
                if (model.move_tab(from, to) && view.moved) view.moved();
                break;
            }
            case WindowCommand::AccountPicker:
                if (view.pickerCommand) view.pickerCommand(key.type);
                break;
            case WindowCommand::AccountCommand:
                if (view.accountCommand) view.accountCommand(key.type);
                break;
            case WindowCommand::Zoom:
                view.askedTextSteps = policy::textZoom(*view.application, {}, view.askedTextSteps,
                    key.argument, -kTextStepsDown, kTextStepsUp);
                break;
            case WindowCommand::OpenTerminal:
                if (view.terminals) {
                    const auto id = model.add_terminal();
                    HitchScope scope(view.hitch, Phase::TerminalTab, std::uint32_t(id));
                    if (!view.terminals->open(id))
                        model.close_tab(model.active());
                }
                break;
            case WindowCommand::OpenTab: {
                auto found = view.windowApp.types.find(key.type);
                if (found != view.windowApp.types.end()) {
                    model.open_extension(key.type, found->second.group, found->second.title);
                    view.extensionProblem.clear();
                }
                break;
            }
            case WindowCommand::CloseTerminal:
            case WindowCommand::CloseTab: {
                if (!model.tab_count()) break;
                const auto& tab = model.tab(model.active());
                const bool terminal = tab.kind == TabKind::Terminal;
                // Older window descriptions used separate primitives for terminals and extensions.
                const bool legacy = key.command == WindowCommand::CloseTerminal;
                if ((legacy && !terminal) || !view.windowApp.closable.value_or(legacy || tab.kind == TabKind::Extension))
                    break;
                const auto id = tab.id;
                if (tab.kind == TabKind::Friend && view.dismissFriend)
                    view.dismissFriend(tab);
                model.dismiss_tab(model.active());
                if (terminal && view.terminals) {
                    HitchScope scope(view.hitch, Phase::TerminalTab, std::uint32_t(id));
                    view.terminals->close(id);
                }
                view.extensionProblem.clear();
                break;
            }
            case WindowCommand::InvokeService:
                if (model.tab_count() && model.tab(model.active()).kind == TabKind::Extension) {
                    const auto& tab = model.tab(model.active());
                    auto type = view.windowApp.types.find(tab.type);
                    if (!view.requestService || type == view.windowApp.types.end() || type->second.service.empty())
                        view.extensionProblem = "Connect to a server and configure a service for this tab type.";
                    else
                        view.extensionProblem = view.requestService(type->second.service, tab.draft);
                }
                break;
            case WindowCommand::Stop:
                if (model.tab_count() && model.tab(model.active()).kind == TabKind::Friend && view.stop)
                    view.stop();
                break;
        }
    }

    void page(PrLayoutNode window, bool composing) {
        updateApplication(); // A shortcut or the strip may have selected another tab this round.
        const auto& panels = view.windowApp.panels;
        if (std::find(panels.begin(), panels.end(), Panel::Board) == panels.end()) {
            view.boardBox = kPrLayoutNodeNone;
            if (view.board)
                view.board->hidden();
        }
        if (std::find(panels.begin(), panels.end(), Panel::Transcript) == panels.end() && view.shown != ~std::size_t(0)) {
            leaveTranscript();
            view.shown = ~std::size_t(0);
            view.transcript = kPrLayoutNodeNone;
        }
        if (std::find(panels.begin(), panels.end(), Panel::Prompt) == panels.end()) {
            view.prompted = ~std::size_t(0);
            view.prompt = kPrLayoutNodeNone;
            view.promptBox = kPrLayoutNodeNone;
        }
        for (const auto panel : panels) {
            switch (panel) {
                case Panel::Board: sprint(window); break;
                case Panel::Terminal: terminal(window, model.tab(model.active())); break;
                case Panel::Transcript: transcript(window); break;
                case Panel::Prompt: prompt(element(window, Part::Footer), composing); break;
                case Panel::Account: accountPanel(window); break;
                case Panel::Native: {
                    const auto& tab = model.tab(model.active());
                    auto type = view.windowApp.types.find(tab.type);
                    if (type != view.windowApp.types.end()) {
                        auto draw = view.nativeTabs.find(type->second.native);
                        if (draw != view.nativeTabs.end()) {
                            draw->second(kit, window, model, asking == nullptr);
                            break;
                        }
                    }
                    caption(element(window, Part::ReplyRow), "The native renderer for this tab is unavailable. Check its registration.",
                        textStyle(PrKitTextRole_Body, kNoticeTone));
                    break;
                }
                case Panel::TextArea:
                    drawTabTextArea(kit, window, model, asking == nullptr);
                    break;
                case Panel::Label:
                    caption(element(window, Part::ReplyRow), view.windowApp.label, textStyle(PrKitTextRole_Body, kChatTone));
                    break;
                case Panel::ServiceReply: {
                    const auto& tab = model.tab(model.active());
                    const auto type = view.windowApp.types.find(tab.type);
                    if (type == view.windowApp.types.end()) break;
                    for (std::size_t i = 0; i < model.tab_count(); ++i) {
                        const auto& service = model.tab(i);
                        if (service.kind == TabKind::Friend && service.name == type->second.service &&
                                !service.transcript.empty()) {
                            const auto& result = service.transcript.back();
                            if (result.kind == EntryKind::Friend || (result.kind == EntryKind::Notice && look.transcriptStyle.showNotices))
                                caption(element(window, Part::ReplyRow), result.text, textStyle(PrKitTextRole_Body,
                                    result.kind == EntryKind::Notice ? kNoticeTone : kChatTone));
                            break;
                        }
                    }
                    break;
                }
                case Panel::Status:
                    caption(element(window, Part::Footer), view.status,
                        textStyle(PrKitTextRole_BodySmall, kStatusTone, PrKitTextAlign_Center));
                    break;
            }
        }
        if (!view.extensionProblem.empty() && model.tab_count() && model.tab(model.active()).kind == TabKind::Extension)
            caption(element(window, Part::ReplyRow), view.extensionProblem, textStyle(PrKitTextRole_BodySmall, kNoticeTone));
    }

};

} // namespace

PrKitTheme windowTheme(PrFontSet* fonts, bool overlay, int steps, float opacity, const MessageCardStyle& card,
                       const TranscriptStyle& transcript) {
    const float k = textScale(steps);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, fonts);
    // The palette is public, but which slot each widget draws with (the tab strip bg1, a hovered tab bg2, the strip's
    // rule line2) is read from the kit's source, not documented: Prax has no public per-widget colours (a request).
    PrKitPalette& p = theme.palette;
    p.bg0 = overlay ? faded(kBackground, opacity) : kBackground;   // the canvas
    p.bg1 = overlay ? faded(kStrip, kOverlayStripFill) : kStrip;            // the tab strip
    p.bg2 = kPromptFill;                                                    // a tab under the pointer
    p.line2 = kStripLine;                                                   // the strip's rule
    // These tones are used by the fork's transcript and status labels. Keep the palette for controls
    // and the inverse ink of the sign-in card independent of the conversation.
    p.axisX = rgb(transcript.ink);
    p.axisW = rgb(transcript.personInk);
    p.axisY = rgb(transcript.noticeInk);
    p.axisZ = rgb(transcript.statusInk);
    p.textInverse = rgb(card.ink);
    p.text1 = kInk;
    p.text3 = kNotice;
    p.text4 = kFaint;
    p.textSelection = kSelection;
    auto type = [&](PrKitTextRole role, float size, float lineHeight, bool mono = false) {
        PrKitType& t = theme.types[role];
        t.mono = mono;
        t.italic = false;
        t.size = size * k;
        t.weight = 400;
        t.lineHeight = lineHeight * k;
        t.letterSpacing = 0;
    };
    type(PrKitTextRole_Display, card.textSize, 1.42f * card.textSize);
    type(PrKitTextRole_Body, kBodySize, kLinePitch);                 // transcript, prompt, tabs
    type(PrKitTextRole_BodySmall, kLabelSize, 1.2f * kLabelSize);    // the notice when there are no friends
    type(PrKitTextRole_Meta, kMetaSize, 1.3f * kMetaSize);           // "queued"
    type(PrKitTextRole_Mono, kToolSize, kToolPitch, true);           // a tool line
    return theme;
}

bool beginWindowFrame(View& view, PrKit* kit, PrFontSet* fonts) {
    Look& look = *view.look;
    const bool scaleChanged = look.textSteps != view.askedTextSteps;
    const bool opacityChanged = look.overlay && look.overlayOpacity != view.windowApp.overlayOpacity;
    const bool cardChanged = look.messageCard != view.windowApp.messageCard;
    const bool transcriptChanged = look.transcriptStyle != view.windowApp.transcriptStyle;
    if (!scaleChanged && !opacityChanged && !cardChanged && !transcriptChanged) return false;
    if (opacityChanged || cardChanged || transcriptChanged) {
        if (look.transcriptStyle.showNotices != view.windowApp.transcriptStyle.showNotices ||
            look.transcriptStyle.showTools != view.windowApp.transcriptStyle.showTools)
            view.items.clear();
        look.transcriptStyle = view.windowApp.transcriptStyle;
        look.messageCard = view.windowApp.messageCard;
        if (opacityChanged) look.overlayOpacity = view.windowApp.overlayOpacity;
        prUiForget(kit->ui, &look);
    }
    look.textSteps = view.askedTextSteps;
    PrKitTheme theme = windowTheme(fonts, look.overlay, look.textSteps, look.overlayOpacity, look.messageCard, look.transcriptStyle);
    prKitSetTheme(kit, &theme);
    if (!scaleChanged) return true;
    if (view.terminals != nullptr)
        view.terminals->setTextScale(textScale(look.textSteps));
    if (view.board != nullptr)
        view.board->setTextScale(textScale(look.textSteps));
    if (view.scaled)
        view.scaled();
    return true;
}

namespace {

// `n` with a comma between each group of three digits: 1048576 is "1,048,576".
std::string grouped(std::size_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return out;
}

} // namespace

std::string cutLine(std::size_t given, std::size_t kept) {
    return grouped(given - kept) + " of " + grouped(given) + " bytes were left out: the prompt holds 32 KB.";
}

std::size_t promptRoom(const View& view, PrUi* ui) {
    const Model& model = *view.model;
    if (model.tab_count() == 0 || model.tab(model.active()).kind != TabKind::Friend) {
        return 0;
    }
    const std::size_t used = model.tab(model.active()).draft.size();
    std::size_t selected = 0;
    if (ui != nullptr && view.prompt != kPrLayoutNodeNone) {
        const PrUiTextSelection s = prUiTextEditSelection(ui, view.prompt);
        const std::size_t from = std::min<std::size_t>(std::min(s.anchor, s.caret), used);
        const std::size_t to = std::min<std::size_t>(std::max(s.anchor, s.caret), used);
        selected = to - from;
    }
    const std::size_t left = used - selected;
    return left < kPromptBytes ? kPromptBytes - left : 0;
}

void promptCut(View& view, std::size_t given, std::size_t kept, bool coming) {
    const Model& model = *view.model;
    if (model.tab_count() == 0) {
        return;
    }
    const Tab& tab = model.tab(model.active());
    view.cut = cutLine(given, kept);
    view.cutTab = tab.id;
    view.cutDraft = tab.draft;
    view.cutComing = coming;
    view.cutFollow = true;
}

PrStringView cutText(PrStringView text, std::size_t room) {
    if (text.size <= room) {
        return text;
    }
    std::size_t end = room;
    // Back off continuation bytes (10xxxxxx) so the cut falls before a character's first byte.
    while (end > 0 && (static_cast<unsigned char>(text.data[end]) & 0xC0) == 0x80) {
        --end;
    }
    return PrStringView{text.data, end};
}

bool windowWantsFrame(const View& view) {
    return view.look->transcriptStyle != view.windowApp.transcriptStyle ||
           view.look->messageCard != view.windowApp.messageCard || view.look->textSteps != view.askedTextSteps ||
           (view.look->overlay && view.look->overlayOpacity != view.windowApp.overlayOpacity) || view.breathing || view.cutComing;
}

void buildWindow(View& view, PrKit* kit, PrLayoutNode parent) {
    Builder b{view, kit, kit->ui, kit->tree, *view.look, *view.model, {}};
    b.build(parent);
}

} // namespace nexus
