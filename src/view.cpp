#include "pr/hash.h"
#include "view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

#include "pr/color_space.h"
#include "pr/draw_list.h"
#include "pr/json.h"
#include "pr/kit_choice.h"
#include "pr/kit_button.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/text_layout.h"
#include "pr/text.h"
#include "pr/ui_records.h"
#include "pr/ui_text_edit.h"
#include "pr/unicode_grapheme.h"

#include "style.h"

namespace nexus {

namespace {

using namespace style;

// Dedicated theme slots for independently configurable app text. Each uses the kit's UI font.
constexpr PrKitTextRole kSelectedTabRole = PrKitTextRole_MonoStrong;
constexpr PrKitTextRole kTabRole = PrKitTextRole_MonoNum;
constexpr PrKitTextRole kAccountRole = PrKitTextRole_Caption;
// The text of a menu item in the kit's dialogs (the /account and /login rows), and the room kept either side of a dialog
// whose rows are cut to fit (accountDialog).
constexpr PrKitTextRole kDialogRowRole = PrKitTextRole_Body;

// ---------------------------------------------------------------------------------------------------------------------
// Records for what no kit part draws: the window's frame and background, the transcript's scroller and column, the chat
// bubbles, the prompt's rounded box, and the rows that place the kit's parts. One part per kind of node, and one recipe
// per group kind that switches on the part. Their text is the kit's (labels in the theme's roles), so no record here
// sets a font.

enum class Part : unsigned char {
    Window,
    Transcript,
    TranscriptAtEnd,
    PanelColumn,
    Column,
    Conversation,
    AssistantMessage,
    Centered,
    Entry,
    Bubble,
    PersonBubble,
    PendingBubble,
    CaptionBubble,
    MessageCard,
    FocusedCard,
    SelectorCard,
    SelectorLayer,
    AIPanels,
    SelectorBackground,
    ActionRow,
    ReplyRow,
    Footer,
    Prompt,
    StripRow,
    TabDivider,
    BoardBox,
    Tools,
    Cursor,
    ScannerSegment,
    Spacer,
    Paragraphs,
};

// The height of the kit text area with one line at text scale `k`, which the plus beside it centres on: the line, the
// area's padding of 4 px above and below (kit_input.cpp kAreaPadding), and its 1 px borders, which do not scale. The
// numbers are the kit's own, copied: Prax has no public query for a text area's height or padding (a request).
float oneLineArea(float k, float linePitch) {
    return linePitch * k + 2 * 4 + 2;
}

// The prompt box's padding above and below the text area, inside its 1 px border, so that with one line it is
// kPromptHeight tall.
float promptPadY(float k, float height, float linePitch) {
    return std::max(0.0f, (height * k - oneLineArea(k, linePitch)) / 2 - 1);
}

float caretInset(const PromptStyle& prompt, float offset, float scale) {
    const float room = std::max(0.f, (prompt.height - prompt.caretHeight) / 2);
    return std::max(0.f, room + offset) * scale;
}

PrColor faded(PrColor c, float alpha) {
    c.a = alpha;
    return c;
}

// Smoothly interpolate normalized keyframes. Repeated values hold a level.
double sampleMotion(const MotionCurve& curve, double time) {
    const auto end = std::lower_bound(curve.begin(), curve.end(), time,
        [](const auto& point, double t) { return point.first < t; });
    if (end == curve.begin()) return end->second;
    if (end == curve.end()) return curve.back().second;
    const auto start = end - 1;
    const double t = (time - start->first) / (end->first - start->first);
    return std::lerp(start->second, end->second, (1 - std::cos(3.141592653589793 * t)) / 2);
}

PrColor linearGain(PrColor color, double gain) {
    const auto lit = [&](float channel) {
        return float(prColorTransferEncodeDouble(PrColorTransfer_Srgb,
            gain * prColorTransferDecodeDouble(PrColorTransfer_Srgb, channel)));
    };
    return {lit(color.r), lit(color.g), lit(color.b), color.a};
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
    int16_t steps;

    void make(const Look& look, PrLayoutBoxStyle& box) const {
        const float k = look.ui.scale(steps);
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
                if (look.transcriptStyle.conversationPanel) {
                    box.width = prLayoutLengthPercent(look.prompt.width);
                    box.margin[PrRectSide_Left] = box.margin[PrRectSide_Right] = prLayoutLengthAuto();
                }
                // At the end, the transcript follows it (prUiFollowEnd), and the items in sight slide as entries arrive:
                // anchoring would hold the view on what showed instead, so it is off there.
                box.overflowAnchor =
                    part == Part::TranscriptAtEnd ? PrLayoutOverflowAnchor_None : PrLayoutOverflowAnchor_Auto;
                break;
            case Part::PanelColumn:
            case Part::Column:
                box.position = PrLayoutPosition_Relative;
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(part == Part::PanelColumn ? 100 : look.prompt.width);
                box.minHeight = prLayoutLengthPercent(100);
                box.margin[PrRectSide_Left] = prLayoutLengthAuto();
                box.margin[PrRectSide_Right] = prLayoutLengthAuto();
                padding(box, look.ui.column_inset * k, 0, 0, 0);
                break;
            case Part::AssistantMessage:
                box.display = PrLayoutDisplay_Block;
                box.width = prLayoutLengthPercent(look.transcriptStyle.replyWidth);
                break;
            case Part::Conversation:
                box.position = PrLayoutPosition_Relative;
                box.display = PrLayoutDisplay_Flex;
                padding(box, look.ui.bubble_padding_y * k, look.ui.bubble_padding_x * k, caretInset(look.prompt, -look.prompt.caretOffsetY, k), look.ui.bubble_padding_x * k);
                break;
            case Part::Centered:
                box.display = PrLayoutDisplay_Flex;
                box.margin[PrRectSide_Top] = prLayoutLengthAuto();
                box.margin[PrRectSide_Bottom] = prLayoutLengthAuto();
                break;
            case Part::Entry:
                // Align outgoing bubbles to the same inset from the panel's right and bottom edges.
                if (look.transcriptStyle.conversationPanel)
                    box.margin[PrRectSide_Right] = px(caretInset(look.prompt, -look.prompt.caretOffsetY, k) - look.ui.bubble_padding_x * k);
                [[fallthrough]];
            case Part::ReplyRow:
                box.display = PrLayoutDisplay_Flex;
                break;
            case Part::ActionRow:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(look.ui.action_width);
                box.margin[PrRectSide_Left] = prLayoutLengthAuto();
                box.margin[PrRectSide_Right] = prLayoutLengthAuto();
                break;
            case Part::AIPanels:
                box.display = PrLayoutDisplay_Flex;
                box.position = PrLayoutPosition_Relative;
                box.minHeight = px(0);
                box.overflowX = box.overflowY = PrLayoutOverflow_Hidden;
                break;
            case Part::SelectorLayer:
                box.display = PrLayoutDisplay_Flex;
                box.position = PrLayoutPosition_Absolute;
                box.zIndexed = 1;
                for (auto& edge : box.inset) edge = px(0);
                break;
            case Part::SelectorBackground:
                box.display = PrLayoutDisplay_Block;
                box.position = PrLayoutPosition_Absolute;
                box.zIndexed = 1;
                box.inset[PrRectSide_Top] = box.inset[PrRectSide_Bottom] = prLayoutLengthPercent(-look.messageCard.verticalExtension);
                box.inset[PrRectSide_Left] = box.inset[PrRectSide_Right] =
                    prLayoutLengthPercent(-look.messageCard.sideExtension);
                break;
            case Part::SelectorCard:
            case Part::MessageCard:
            case Part::FocusedCard:
                box.display = PrLayoutDisplay_Flex;
                box.minWidth = px(0);
                box.maxWidth = prLayoutLengthPercent(100);
                padding(box, look.ui.bubble_padding_y * k, look.ui.bubble_padding_x * k, look.ui.bubble_padding_y * k, look.ui.bubble_padding_x * k);
                if (part == Part::SelectorCard) {
                    box.width = px(look.messageCard.selectorWidthEm * look.ui.body_size * k);
                    box.maxWidth = prLayoutLengthPercent(100 / (1 + 2 * look.messageCard.sideExtension / 100));
                    box.position = PrLayoutPosition_Relative;
                    box.zIndexed = 1;
                    box.padding[PrRectSide_Left] = px(look.ui.selector_padding_lines * look.ui.line_pitch * k);
                    box.padding[PrRectSide_Right] = box.padding[PrRectSide_Left];
                }
                break;
            case Part::PendingBubble:
            case Part::PersonBubble:
            case Part::Bubble:
                box.position = PrLayoutPosition_Relative;
                box.zIndexed = 1;
                box.display = PrLayoutDisplay_Block;
                box.maxWidth = prLayoutLengthPercent(part == Part::Bubble ? look.ui.bubble_max_percent : look.ui.person_bubble_max_percent);
                padding(box, look.ui.bubble_padding_y * k, look.ui.bubble_padding_x * k, look.ui.bubble_padding_y * k, look.ui.bubble_padding_x * k);
                break;
            case Part::CaptionBubble:
                box.display = PrLayoutDisplay_Block;
                box.maxWidth = prLayoutLengthPercent(100);
                padding(box, look.transcriptStyle.captionPaddingY * k, look.transcriptStyle.captionPaddingX * k,
                    look.transcriptStyle.captionPaddingY * k, look.transcriptStyle.captionPaddingX * k);
                break;
            case Part::Footer:
                box.display = PrLayoutDisplay_Flex;
                padding(box, look.ui.footer_padding * k, 0, look.ui.footer_padding * k, 0);
                break;
            case Part::Prompt:
                box.position = PrLayoutPosition_Relative;
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(look.prompt.width);
                padding(box, promptPadY(k, look.prompt.height, look.ui.line_pitch), look.prompt.paddingX * k, promptPadY(k, look.prompt.height, look.ui.line_pitch), look.prompt.paddingX * k);
                for (PrLayoutUnit& b : box.border)
                    b = prLayoutUnitFromPx(look.ui.prompt_border_width);
                break;
            case Part::TabDivider:
                box.height = px(look.ui.tab_divider_height);
                break;
            case Part::StripRow:
                box.display = PrLayoutDisplay_Flex;
                box.height = px(look.tabs.height * k + look.ui.strip_border_height);
                break;
            case Part::BoardBox:
                box.display = PrLayoutDisplay_Flex;
                box.minHeight = px(0);
                box.overflowX = PrLayoutOverflow_Hidden;
                box.overflowY = PrLayoutOverflow_Hidden;
                break;
            case Part::Tools:
                box.position = PrLayoutPosition_Relative;
                box.display = PrLayoutDisplay_Flex;
                box.maxWidth = prLayoutLengthPercent(look.ui.bubble_max_percent);
                padding(box, look.transcriptStyle.captionPaddingY * k, look.transcriptStyle.captionPaddingX * k,
                    look.transcriptStyle.captionPaddingY * k, look.transcriptStyle.captionPaddingX * k);
                if (look.transcriptStyle.toolChevron)
                    box.padding[PrRectSide_Left] = px((look.transcriptStyle.captionPaddingX +
                        look.transcriptStyle.toolChevronSize + look.transcriptStyle.toolChevronGap) * k);
                box.minWidth = px(0);
                box.overflowX = PrLayoutOverflow_Hidden;
                break;
            case Part::ScannerSegment:
                box.display = PrLayoutDisplay_Block;
                box.width = px(look.prompt.segmentSize(k).x);
                box.height = px(look.prompt.segmentSize(k).y);
                break;
            case Part::Cursor:
                // Just under the last text: the column's gap less look.ui.cursor_gap is taken back above it.
                box.display = PrLayoutDisplay_Block;
                box.width = px(look.prompt.segmentSize(k).x);
                box.height = px(look.prompt.segmentSize(k).y);
                box.margin[PrRectSide_Top] = px(-(look.ui.entry_gap - look.ui.cursor_gap) * k);
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
    int16_t steps;

    void make(const Look& look, PrLayoutDecorationStyle& d) const {
        const float k = look.ui.scale(steps);
        switch (part) {
            case Part::Window:
                d.background = prColor16FromColor(look.overlay ? faded(rgb(look.colors.background), look.overlayOpacity) : rgb(look.colors.background));
                break;
            case Part::Transcript:
            case Part::TranscriptAtEnd:
                if (look.flags.opaque_history_backdrop)
                    d.background = prColor16FromColor(rgb(look.colors.background));
                // The lower edge stays in place while the top of the conversation scrolls with history.
                if (look.transcriptStyle.conversationPanel) {
                    for (const auto corner : {PrRectCorner_BottomLeft, PrRectCorner_BottomRight})
                        d.radii[2 * corner] = d.radii[2 * corner + 1] = px(look.transcriptStyle.radius * k);
                }
                break;
            case Part::Conversation:
                d.background = prColor16FromColor(rgb(look.transcriptStyle.panelFill.value_or(look.tabs.fill)));
                radius(d, look.transcriptStyle.radius * k);
                break;
            case Part::TabDivider:
                d.background = prColor16FromColor(rgb(look.tabs.divider));
                break;
            case Part::StripRow:
                d.background = prColor16FromColor(faded(rgb(look.tabs.fill), look.tabs.opacity));
                break;
            case Part::SelectorBackground:
            case Part::SelectorCard:
            case Part::MessageCard:
            case Part::FocusedCard:
                d.background = prColor16FromColor(rgb(look.messageCard.fill));
                radius(d, look.messageCard.radius * k);
                if (part == Part::FocusedCard) {
                    d.outlineStyle = PrDrawBorderStyle_Solid;
                    d.outlineColor = prColor16FromColor(rgb(look.messageCard.ink));
                    d.outlineWidth = prLayoutUnitFromPx(look.ui.focused_outline_width * k);
                    d.outlineOffset = prLayoutUnitFromPx(-look.ui.focused_outline_inset * k);
                }
                break;
            case Part::PendingBubble:
            case Part::PersonBubble:
            case Part::Bubble:
            case Part::CaptionBubble:
            case Part::Tools:
                d.background = prColor16FromColor(rgb(look.transcriptStyle.fill));
                radius(d, look.transcriptStyle.radius * k);
                if ((part == Part::Bubble || part == Part::PersonBubble) && look.transcriptStyle.sentOutlineWidth > 0) {
                    d.outlineStyle = PrDrawBorderStyle_Solid;
                    d.outlineColor = prColor16FromColor(rgb(look.transcriptStyle.sentOutlineInk));
                    d.outlineWidth = prLayoutUnitFromPx(look.transcriptStyle.sentOutlineWidth * k);
                    d.outlineOffset = -d.outlineWidth;
                }
                break;
            case Part::Prompt:
                d.background = prColor16FromColor(rgb(look.prompt.fill));
                for (int side = 0; side < PrRectSide_Count; ++side) {
                    d.borderColor[side] = prColor16FromColor(rgb(look.prompt.border));
                    d.borderStyle[side] = PrDrawBorderStyle_Solid;
                }
                radius(d, look.prompt.radius * k);
                break;
            default:
                break;
        }
    }
};

struct FlexRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    Part part;
    int16_t steps;

    void make(const Look& look, PrLayoutFlexStyle& flex) const {
        const float k = look.ui.scale(steps);
        switch (part) {
            case Part::Window:
                flex.direction = PrLayoutFlexDirection_Column;
                break;
            case Part::AIPanels:
            case Part::Transcript:
            case Part::TranscriptAtEnd:
                // It starts at the newest entry and stays there as entries arrive (prUiFollowEnd, in transcript()), and
                // scroll anchoring holds a view scrolled up in place.
                flex.direction = PrLayoutFlexDirection_Column;
                flex.grow = 1;
                flex.basis = px(0);
                break;
            case Part::Conversation:
            case Part::PanelColumn:
            case Part::Column:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.justifyContent = PrLayoutAlignment_FlexEnd;
                flex.rowGap = px(look.ui.entry_gap * k);
                flex.shrink = 0;
                break;
            case Part::Centered:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.alignItems = PrLayoutAlignment_Center;
                break;
            case Part::SelectorLayer:
            case Part::ActionRow:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.grow = 1;
                flex.basis = px(0);
                flex.alignItems = PrLayoutAlignment_Center;
                flex.justifyContent = PrLayoutAlignment_Center;
                break;
            case Part::SelectorCard:
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
                flex.columnGap = px(look.prompt.gap * k);
                flex.shrink = 0;
                break;
            case Part::TabDivider:
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
                flex.rowGap = px(look.transcriptStyle.toolGap * k);
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
                flex.rowGap = px(look.ui.line_pitch * k);
                flex.shrink = 0;
                break;
            default:
                break;
        }
    }
};

struct StackLevel {
    int level;
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    void make(const Look&, PrLayoutEffectStyle& effect) const { effect.zIndex = level; }
};

PrLayoutStyleIndex style(PrUi* ui, const Look& look, Part p) {
    int16_t s = static_cast<int16_t>(look.textSteps);
    if (p == Part::SelectorLayer)
        return pr::uiStyle(ui, look, BoxRecipe{p, s}, FlexRecipe{p, s}, StackLevel{2});
    if (p == Part::SelectorBackground)
        return pr::uiStyle(ui, look, BoxRecipe{p, s}, DecorationRecipe{p, s}, StackLevel{-1});
    if (p == Part::PendingBubble || p == Part::Bubble || p == Part::PersonBubble)
        return pr::uiStyle(ui, look, BoxRecipe{p, s}, DecorationRecipe{p, s}, StackLevel{1});
    return pr::uiStyle(ui, look, BoxRecipe{p, s}, DecorationRecipe{p, s}, FlexRecipe{p, s});
}

// The state cursor's fill: its state's OKLCH colour (cursor.h) at a breath level, in sRGB for the layout. One record
// per state and level (kBreathLevels for the busy one), so the breathe makes a bounded set of records.
struct CursorRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    unsigned char state;
    unsigned char level;
    int16_t steps;

    void make(const Look& look, PrLayoutDecorationStyle& d) const {
        Oklch c{};
        const auto& s = look.ui;
        switch (CursorState(state)) {
            case CursorState::Busy: {
                const float f = float(std::clamp(int(level), 0, kBreathLevels - 1)) / (kBreathLevels - 1);
                c = {std::lerp(s.cursor_busy_dim_lightness, s.cursor_busy_bright_lightness, f),
                     std::lerp(s.cursor_busy_dim_chroma, s.cursor_busy_bright_chroma, f),
                     std::lerp(s.cursor_busy_dim_hue, s.cursor_busy_bright_hue, f)};
                break;
            }
            case CursorState::Working: c = {s.cursor_working_lightness, s.cursor_working_chroma, s.cursor_working_hue}; break;
            case CursorState::Idle: c = {s.cursor_idle_lightness, s.cursor_idle_chroma, s.cursor_idle_hue}; break;
            case CursorState::Down: c = {s.cursor_down_lightness, s.cursor_down_chroma, s.cursor_down_hue}; break;
            case CursorState::Lost: c = {s.cursor_lost_lightness, s.cursor_lost_chroma, s.cursor_lost_hue}; break;
            case CursorState::None: break;
        }
        const double in[3] = {c.l, c.c, c.h};
        double out[3] = {};
        prColorConvert(PrColorSpace_Oklch, PrColorSpace_Srgb, in, out);
        auto channel = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
        d.background = prColor16FromColor(PrColor{channel(out[0]), channel(out[1]), channel(out[2]), 1});
        radius(d, look.prompt.caretRadius * look.ui.scale(steps));
    }
};


// Quantize light, not position, so adjacent lamps crossfade and the style cache stays bounded.
struct ScannerLight {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    uint8_t level;
    int16_t steps;
    uint16_t peak; // linear-light gain in sixty-fourths
    uint8_t activity;

    void make(const Look& look, PrLayoutDecorationStyle& d) const {
        const auto off = rgb(look.prompt.fill);
        const auto on = rgb(look.transcriptStyle.scannerInk.value_or(look.tabs.selectedFill));
        const double gain = std::lerp(double(look.ui.scanner_dim_gain), peak / 64.0, level / 255.0);
        auto channel = [&](float a, float b) {
            if (activity == 0) return a;
            const auto idle = prColorTransferDecodeDouble(PrColorTransfer_Srgb, a);
            const auto lit = prColorTransferDecodeDouble(PrColorTransfer_Srgb, b) * gain;
            return float(prColorTransferEncodeDouble(PrColorTransfer_Srgb, std::lerp(idle, lit, activity / 255.0)));
        };
        d.background = prColor16FromColor({channel(off.r,on.r), channel(off.g,on.g), channel(off.b,on.b), 1});
        radius(d, look.prompt.caretRadius * look.ui.scale(steps));
    }
};

PrLayoutStyleIndex cursorStyle(PrUi* ui, const Look& look, CursorState state, int level) {
    int16_t s = static_cast<int16_t>(look.textSteps);
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
constexpr PrKitTone kChatTone = PrKitTone_AxisX;
constexpr PrKitTone kPersonTone = PrKitTone_AxisW;
constexpr PrKitTone kNoticeTone = PrKitTone_AxisY;
constexpr PrKitTone kStatusTone = PrKitTone_AxisZ;

void paintPendingOutline(void* context, PrVec2 size, PrDrawList* list) {
    const auto& view = *static_cast<View*>(context);
    const auto& look = *view.look;
    const float scale = look.ui.scale(look.textSteps);
    const float width = look.transcriptStyle.pendingOutlineWidth * scale;
    const float inset = width * 0.5f;
    if (size.x <= width || size.y <= width || width <= 0) return;
    const float pattern[] = {look.ui.pending_dash_length * width, look.ui.pending_dash_gap * width};
    auto stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.shape.dashes = pattern;
    stroke.shape.dashCount = 2;
    stroke.shape.dashOffset = -5 * width * view.pendingDashStep / 64;
    PrDrawPaint paint{};
    paint.color = prColor16FromColor(rgb(look.tabs.selectedFill));
    paint.stroke = &stroke;
    prDrawListCornerRect(list, prCornerRectRound({inset, inset, size.x - inset, size.y - inset},
        std::max(0.f, look.transcriptStyle.radius * scale - inset)), paint);
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
        return look.ui.scale(look.textSteps);
    }

    PrLayoutNode element(PrLayoutNode parent, Part p, PrUiFlags flags = 0, PrUiKey key = 0) {
        return prUiElement(ui, parent, key, style(ui, look, p), flags);
    }

    PrLayoutNode label(PrLayoutNode parent, std::string_view text, PrKitTextStyle s) {
        return prKitLabel(kit, parent, PrKitLabel{.key = 0, .text = sv(text)}, s);
    }

    void ink(PrLayoutNode node, unsigned color, bool inlineBox = false, double gain = 1) {
        auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, node));
        if (inlineBox) record.groups[PrLayoutGroup_Box] = prLayoutStyleDefault().groups[PrLayoutGroup_Box];
        auto& group = record.groups[PrLayoutGroup_Color];
        auto colors = prLayoutColorStyleRecord(tree, group);
        colors.color = prColor16FromColor(gain == 1 ? rgb(color) : linearGain(rgb(color), gain));
        group = prLayoutColorStyleIntern(tree, &colors);
        prLayoutTreeFrameSetStyle(tree, node, prLayoutStyleIntern(tree, &record));
    }

    void caption(PrLayoutNode parent, std::string_view text, PrKitTextStyle appearance) {
        if (!text.empty())
            label(element(parent, Part::CaptionBubble), text, appearance);
    }

    void limitPanel(PrLayoutNode parent) {
        const auto& app = view.windowApp;
        if (!app.limitOffer.request) return;
        const auto card = element(element(parent, Part::Footer), Part::CaptionBubble);
        auto body = textStyle(PrKitTextRole_Body, kChatTone);
        body.wrap = PrKitWrap_Wrap;
        label(card, app.label, body);
        auto option = [&](const char* choice, const std::string& target, const std::string& title) {
            if (target.empty() || title.empty() || app.limitOffer.waiting || !app.limitOffer.connected) return;
            const auto id = std::to_string(app.limitOffer.session) + "/" + std::to_string(app.limitOffer.request) +
                "/" + choice + "/" + target;
            const auto button = prKitButton(kit, card, {.key = prHashString(sv(id), 0), .text = sv(title)}, {});
            if (!asking && !view.accountPicker && view.displayedLimit == app.limitOffer &&
                    prKitActivated(kit, button) && view.resolveLimit)
                view.resolveLimit(app.limitOffer, choice);
        };
        option("account", app.limitOffer.account, app.limitAccountLabel);
        option("model", app.limitOffer.model, app.limitModelLabel);
    }

    bool uiKey(PrLayoutNode node, std::string_view chord, bool capture = true) {
        PrAppKey key{};
        PrAppModifierFlags modifiers{};
        if (!uiChord(chord, key, modifiers)) return false;
        return capture ? prUiKeyCaptured(ui, node, key, modifiers) : prUiKeyPressed(ui, node, key, modifiers);
    }

    bool idleAnimation(double since) const {
        const auto& settings = look.transcriptStyle;
        return settings.idleAnimations && !prUiUserPreferences(ui).reducedMotion &&
            (!view.savingPower || settings.batteryAnimationIdleSeconds == 0 ||
             view.time - std::max(since, view.lastInput) < settings.batteryAnimationIdleSeconds);
    }

    PrColor highlight(unsigned color, float opacity, double since) {
        auto fill = faded(rgb(color), opacity);
        const auto display = prUiDisplay(ui);
        if (!idleAnimation(since) || look.transcriptStyle.highlightEnergy == 0)
            return fill;
        view.energyAnimating = true;
        const double age = std::max(0.0, view.time - since);
        const double phase = view.time * 6.283185307179586 / look.transcriptStyle.highlightSeconds;
        const double flash = std::exp(-age / look.transcriptStyle.flashSeconds);
        if (display.headroom <= 1) {
            fill.a = std::min(1.0f, opacity * float(1 + look.transcriptStyle.highlightEnergy *
                (look.ui.highlight_sdr_pulse * std::sin(phase) + look.ui.highlight_sdr_flash * flash)));
            return fill;
        }
        const double pulse = look.ui.highlight_hdr_floor + look.ui.highlight_hdr_pulse * std::cos(phase);
        const double lift = (pulse + look.ui.highlight_hdr_flash * flash) * look.transcriptStyle.highlightEnergy;
        // Quantize the linear-light gain to bound the number of interned paint records.
        const double gain = std::round(128 * (1 + lift * std::min(1.0f, (display.headroom - 1) * 4))) / 128;
        return linearGain(fill, gain);
    }

    void selectorAction(const std::string& action) {
        if (!model.tab_count()) return;
        const auto active = model.active();
        if (view.dialogVisible && (action == "picker:close" || action == "dismiss"))
            view.chirp(view.windowApp.surface.cancelChirp);
        if (action == "picker:model" || action == "picker:effort") {
            view.choiceKind = action.substr(7);
            view.choiceTab = model.tab(active).id;
            view.choiceValue.clear();
            model.set_draft(active, {});
        } else if (action == "picker:close") {
            view.choiceKind.clear();
            view.choiceValue.clear();
        } else if (action.starts_with("point:") && !view.choiceKind.empty()) {
            view.choiceValue = action.substr(6);
        } else if (action.starts_with("choose:") && !view.choiceKind.empty()) {
            view.chirp(view.windowApp.surface.confirmChirp);
            model.set_draft(active, "/" + view.choiceKind + " " + action.substr(std::string_view("choose:").size()));
            view.choiceKind.clear();
            view.choiceValue.clear();
            updateApplication();
            promptKey(active, "Enter");
        } else if (action.starts_with("login:") && view.accounts.dialog == AccountMenuView::Dialog::Login) {
            view.accounts.dialog = AccountMenuView::Dialog::None;
            if (view.loginAccount) view.loginAccount(action.substr(6));
        } else if (view.accountAction) {
            view.accountAction(action);
        }
    }

    void accountSelector(PrLayoutNode parent, const std::vector<AccountItem>& items, bool modal = false) {
        if (items.empty()) return;
        const auto layer = modal ? parent : element(parent, Part::SelectorLayer);
        if (!modal && view.windowApp.surface.showTabs) {
            // Balance the strip above the panels while keeping its hit area free.
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, layer));
            auto& group = record.groups[PrLayoutGroup_Box];
            auto box = prLayoutBoxStyleRecord(tree, group);
            box.padding[PrRectSide_Bottom] = px(view.headerHeight);
            group = prLayoutBoxStyleIntern(tree, &box);
            prLayoutTreeFrameSetStyle(tree, layer, prLayoutStyleIntern(tree, &record));
        }
        const auto card = modal ? parent : element(layer, Part::SelectorCard);
        if (modal) {
            auto record = prLayoutStyleRecord(tree, style(ui, look, Part::SelectorCard));
            auto& group = record.groups[PrLayoutGroup_Box];
            auto box = prLayoutBoxStyleRecord(tree, group);
            // A modal owns a separate viewport: resolve the card's percentage against the window, not itself.
            const float width = std::min(look.messageCard.selectorWidthEm * look.ui.body_size * k(),
                view.extent.x / (1 + 2 * look.messageCard.sideExtension / 100));
            box.width = box.maxWidth = px(width);
            group = prLayoutBoxStyleIntern(tree, &box);
            prLayoutTreeFrameSetStyle(tree, card, prLayoutStyleIntern(tree, &record));
        }
        view.accountMenu = card;
        element(card, Part::SelectorBackground);
        const float lineHeight = kit->theme.types[PrKitTextRole_Body].lineHeight;
        bool afterHeading = false;
        for (const auto& item : items) {
            if (item.action.empty()) {
                label(card, item.text, textStyle(PrKitTextRole_Display, PrKitTone_Inverse, PrKitTextAlign_Center));
                afterHeading = true;
                continue;
            }
            const auto row = prKitButton(kit, card, {.key = prHashString(sv(item.action), 0)},
                                         {.variant = PrKitButtonVariant_Ghost});
            PrKitPlace place{};
            place.height = prKitPx(lineHeight * look.messageCard.rowHeight);
            if (afterHeading) place.margin.top = look.messageCard.headingGap * lineHeight;
            else if (!view.accountRows.empty()) place.margin.top = look.messageCard.rowGap * lineHeight;
            afterHeading = false;
            place.align = PrKitAlign_Stretch;
            prKitPlace(kit, row, place);
            const auto email = label(row, item.text, textStyle(static_cast<PrKitTextRole>(item.selected ? PrKitTextRole_Strong : kAccountRole), PrKitTone_Inverse, PrKitTextAlign_Center));
            view.accountRows.push_back(row);
            if (item.selected) {
                auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, row));
                auto& group = record.groups[PrLayoutGroup_Decoration];
                auto decoration = prLayoutDecorationStyleRecord(tree, group);
                if (view.energyAccount != item.action) {
                    if (view.dialogVisible && !view.energyAccount.empty())
                        view.chirp(view.windowApp.surface.stepChirp);
                    view.accountEnergyTime = view.time;
                    view.energyAccount = item.action;
                }
                decoration.background = prColor16FromColor(highlight(look.messageCard.selectedFill, look.messageCard.selectedOpacity, view.accountEnergyTime));
                decoration.outlineWidth = prLayoutUnitFromPx(0);
                radius(decoration, look.messageCard.selectedRadius * k());
                group = prLayoutDecorationStyleIntern(tree, &decoration);
                prLayoutTreeFrameSetStyle(tree, row, prLayoutStyleIntern(tree, &record));
                auto emailStyle = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, email));
                auto& colors = emailStyle.groups[PrLayoutGroup_Color];
                auto ink = prLayoutColorStyleRecord(tree, colors);
                ink.color = prColor16FromColor(rgb(look.messageCard.selectedInk));
                colors = prLayoutColorStyleIntern(tree, &ink);
                prLayoutTreeFrameSetStyle(tree, email, prLayoutStyleIntern(tree, &emailStyle));
                if ((modal || view.accountPicker || !view.choiceKind.empty()) && view.lastHighlightedAccount != item.action) {
                    prUiSetFocus(ui, row);
                    view.lastHighlightedAccount = item.action;
                }
            }
            if (prKitActivated(kit, row)) selectorAction(item.action);
        }
    }

    void signInFailure(PrLayoutNode parent, const SignInAccount& account) {
        if (account.email.empty() || !model.tab_count()) return;
        const auto& entries = model.tab(model.active()).transcript;
        const auto needed = account.name + " needs signing in:";
        const auto failure = "Could not sign in to " + account.email + ". ";
        // A failed browser return queues a fresh offer immediately after its failure notice.
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->kind != EntryKind::Notice) break;
            if (it->text.starts_with(needed)) continue;
            if (it->text.starts_with(failure)) {
                auto appearance = textStyle(PrKitTextRole_BodySmall, kNoticeTone, PrKitTextAlign_Center);
                appearance.wrap = PrKitWrap_Wrap;
                caption(parent, it->text, appearance);
            }
            break;
        }
    }

    void accountPanel(PrLayoutNode parent) {
        if (view.accounts.dialog == AccountMenuView::Dialog::Login) return;
        const auto& selected = view.windowApp.signInAccount;
        if (view.windowApp.surface.items && !asking) {
            accountSelector(parent, *view.windowApp.surface.items);
            if (view.accountMenu != kPrLayoutNodeNone && view.choiceKind.empty())
                signInFailure(view.accountMenu, selected);
            return;
        }
        if (selected.name.empty()) return;
        const auto footer = element(parent, Part::ActionRow);
        const auto button = prKitButton(kit, footer, {.key = prHashString(sv("sign-in-action"), 0)}, {});
        const auto appearance = prUiFocus(ui) == button || prUiIsHovered(ui, button) ?
            Part::FocusedCard : Part::MessageCard;
        prLayoutTreeFrameSetStyle(tree, button, style(ui, look, appearance));
        auto large = textStyle(PrKitTextRole_Display, PrKitTone_Inverse, PrKitTextAlign_Center);
        large.wrap = PrKitWrap_Wrap;
        label(button, uiFormat(look.words.sign_in_action, {{"account", selected.label.empty() ? selected.name : selected.label}}), large);
        if (!selected.email.empty()) {
            const auto email = label(button, selected.email, large);
            // Keep the Kit label's typography and wrapping. Its color belongs only to this address.
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, email));
            auto ink = prLayoutColorStyleRecord(tree, record.groups[PrLayoutGroup_Color]);
            ink.color = prColor16FromColor(rgb(look.messageCard.emailInk));
            record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &ink);
            prLayoutTreeFrameSetStyle(tree, email, prLayoutStyleIntern(tree, &record));
        }
        signInFailure(button, selected);
        label(button, look.words.sign_in_hint, textStyle(PrKitTextRole_BodySmall, PrKitTone_Inverse, PrKitTextAlign_Center));
        if (prKitActivated(kit, button) && view.activateSignIn)
            view.activateSignIn();
    }

    bool showsPromptPlus(const Tab& tab) const {
        return look.prompt.showPlus && !tab.primaryChat();
    }

    // The kit's plus, centred on the prompt's last line, where the box aligns its items at the bottom.
    void plus(PrLayoutNode box) {
        const float side = look.ui.plus_size * k();
        PrLayoutNode node = prKitIcon(kit, box, PrKitIcon{.key = 0, .icon = PrStudioIconKind_Plus},
            PrKitIconStyle{.size = side, .tone = PrKitTone_Primary, ._pad = {}});
        PrKitPlace place{};
        place.margin.bottom = (oneLineArea(k(), look.ui.line_pitch) - side) / 2;
        place.flex = PrKitFlex_Fixed;
        prKitPlace(kit, node, place);
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
        auto& group = record.groups[PrLayoutGroup_Box];
        auto box = prLayoutBoxStyleRecord(tree, group);
        box.padding[PrRectSide_Left] = px(0);
        box.border[PrRectSide_Left] = prLayoutUnitFromPx(0);
        group = prLayoutBoxStyleIntern(tree, &box);
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
        const auto count = end - first;
        const auto activeIndex = model.active();
        const auto selection = activeIndex >= first && activeIndex < end ? kitId(model.tab(activeIndex)) : kNoTab;
        if (prKitTabsCount(tabs) == count + 1 && prKitTabsAt(tabs, uint32_t(count)) == kNoTab) {
            std::size_t same = 0;
            while (same < count && prKitTabsAt(tabs, uint32_t(same)) == kitId(model.tab(first + same)))
                ++same;
            if (same == count) {
                prKitTabsSelect(tabs, selection);
                return;
            }
        }
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
        prKitTabsSelect(tabs, selection);
    }

    // A drop within a strip is applied by the kit as the strip is declared, and is not reported (kit_choice.cpp
    // prKitTabStrip 738-746; prKitTabMoved reports only moves between strips): it is read back here from the kit's
    // order, and made through the model's move_tab. The model refuses a move across zones (a terminal among the
    // AIs, or an AI among the terminals: the kit has no way to keep its caret out of another zone, and no per-tab
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

    // The account control fits beneath the AI title. Only the popover floats outside the strip.
    void accountDropdown(PrLayoutNode title, const std::string& current) {
        view.accountAnchor = prKitButton(kit, title, {.key = prHashString(sv("account"), 0)},
                                        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
        PrKitPlace size{};
        size.height = prKitPx(kit->theme.types[PrKitTextRole_Meta].lineHeight + look.ui.account_anchor_extra_height * k());
        prKitPlace(kit, view.accountAnchor, size);
        auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, view.accountAnchor));
        auto& group = record.groups[PrLayoutGroup_Decoration];
        auto decoration = prLayoutDecorationStyleRecord(tree, group);
        decoration.background = prColor16FromColor(rgb(look.transcriptStyle.fill));
        group = prLayoutDecorationStyleIntern(tree, &decoration);
        prLayoutTreeFrameSetStyle(tree, view.accountAnchor, prLayoutStyleIntern(tree, &record));
        label(view.accountAnchor, current, textStyle(PrKitTextRole_Meta, kStatusTone));
        prKitIcon(kit, view.accountAnchor, {.icon = PrStudioIconKind_ChevronDown},
                  {.size = look.ui.meta_size * k() * look.ui.account_chevron_scale, .tone = kStatusTone, ._pad = {}});
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
        accountRows(popover);
    }

    void accountRows(PrLayoutNode parent) {
        // The fork handles arrow keys and Enter. Native focus follows that preview and remains available to mouse input.
        view.accountMenu = prKitMenu(kit, parent, {}, {.leavesFocus = true});
        captureShortcuts(view.accountMenu);
        if (!view.accountPicker) return;
        const auto& picker = *view.accountPicker;
        std::string chosen;
        for (std::size_t i = 0; i < picker.options.size(); ++i) {
            const auto& option = picker.options[i];
            const auto label = picker.rowLabel(option, view.windowApp.autoAccountLabel);
            const bool preview = i == picker.highlighted;
            const auto row = prKitMenuItem(kit, view.accountMenu,
                {.key = prHashString(sv(option.name), 0), .text = sv(label),
                 .icon = PrStudioIconKind(preview ? PrStudioIconKind_ChevronRight : PrStudioIconKind_None),
                 .checked = option.name == picker.current}, {});
            PrKitPlace rowSize{};
            rowSize.height = prKitPx(kit->theme.types[PrKitTextRole_Body].lineHeight + look.ui.account_row_extra_height * k());
            prKitPlace(kit, row, rowSize);
            view.accountRows.push_back(row);
            if (preview && view.lastHighlightedAccount != option.name) {
                if (view.dialogVisible && !view.lastHighlightedAccount.empty())
                    view.chirp(view.windowApp.surface.stepChirp);
                prUiSetFocus(ui, row);
                prUiReveal(ui, row);
                view.lastHighlightedAccount = option.name;
            }
            if (prKitActivated(kit, row)) chosen = option.name;
        }
        if (!chosen.empty() && view.chooseAccount) view.chooseAccount(chosen);
    }

    float tabAdvance(std::size_t index, const std::string& title) {
        auto& cached = view.tabWidths[index];
        if (cached.title != title) {
            if (!view.windowShaper && !fallbackTabShaper)
                fallbackTabShaper.reset(prTextShaperCreate(PrTextShaperBackend_Default));
            auto* shaper = view.windowShaper ? view.windowShaper : fallbackTabShaper.get();
            if (!tabLayout) tabLayout.reset(prTextLayoutCreate());
            cached = {title, 0};
            for (const auto role : {kTabRole, kSelectedTabRole}) {
                if (!shaper || !kit->fonts[role]) continue;
                const PrTextLayoutDesc desc{.text = sv(title), .font = kit->fonts[role], .shaper = shaper};
                prTextLayoutSet(tabLayout.get(), &desc);
                cached.advance = std::max(cached.advance, std::ceil(prTextLayoutExtent(tabLayout.get()).x));
            }
        }
        return cached.advance;
    }

    // A dialog that is typed into: at the window's top where the view asks for it (View::dialogsAtTop, iOS), where the
    // on-screen keyboard never covers it; centred otherwise. A modal is placed over the whole window, which knows
    // nothing of the keyboard's room.
    PrKitDialogStyle typedDialogStyle() const {
        PrKitDialogStyle style{};
        style.width = look.ui.dialog_width * k();
        style.modal.inset = look.ui.dialog_inset * k();
        if (view.dialogsAtTop) style.modal.placement = PrKitModalPlacement_Top;
        return style;
    }

    PrVec2 boardExtent() const {
        return {view.extent.x, std::max(0.0f, view.extent.y - view.headerHeight)};
    }

    void select(std::size_t index) {
        if (index >= model.tab_count()) return;
        const auto& tab = model.tab(index);
        if (view.board && tab.kind == TabKind::Sprint && model.active() != index) {
            if (view.pendingBoardId != tab.id) view.board->prepare(boardExtent());
            view.pendingBoardId = tab.id;
            if (!view.board->prepared()) { prUiRequestFrame(ui); return; }
        } else if (view.board && tab.kind == TabKind::Sprint) {
            view.board->home();
        }
        if (view.pendingBoardId && view.board) view.board->cancelPreparation();
        view.pendingBoardId.reset();
        model.select_tab(index);
    }

    void strip(PrLayoutNode parent, PrKitTabs* tabs, std::size_t first, std::size_t end, const std::string& group,
            bool exchanges, PrKitFlex flex) {
        syncTabs(tabs, first, end);
        PrLayoutNode row = prKitTabStrip(kit, parent, PrKitTabStrip{.key = prHashString(sv(group), 0), .tabs = tabs,
            .group = exchanges ? 0x4e78546162477270ull : 0, .reorderable = view.windowApp.surface.reorderTabs, .collapsesSingle = false, .disabled = false},
            PrKitChoiceStyle{.size = PrKitControlSize_Lg, .direction = PrKitDirection_Row});
        PrKitPlace rowPlace{};
        rowPlace.height = prKitPx(look.tabs.height * k() + look.ui.strip_border_height);
        rowPlace.flex = flex;
        prKitPlace(kit, row, rowPlace);
        PrKitTabMove moved{};
        if (prKitTabMoved(kit, row, &moved) && moved.kind == PrKitTabMoveKind_Received && moved.id == view.draggingTab)
            drop = Drop{moved.id, group, moved.index};
        mirrorMove(tabs, first, end);
        const uint64_t selected = prKitTabsSelected(tabs);
        for (std::size_t i = first; i < end; ++i) {
            PrLayoutNode tab = prKitTab(kit, row, PrKitTab{.id = kitId(model.tab(i)), .disabled = false});
            const bool active = i == model.active();
            if (active) {
                auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, tab));
                auto& decorationGroup = record.groups[PrLayoutGroup_Decoration];
                auto decoration = prLayoutDecorationStyleRecord(tree, decorationGroup);
                if (view.energyTab != model.tab(i).id) {
                    view.tabEnergyTime = view.time;
                    view.energyTab = model.tab(i).id;
                    view.energyAccount.clear();
                }
                decoration.background = prColor16FromColor(highlight(look.tabs.selectedFill, look.tabs.selectedOpacity, view.tabEnergyTime));
                decoration.borderStyle[PrRectSide_Bottom] = PrDrawBorderStyle_None;
                radius(decoration, look.tabs.selectedRadius * k());
                decorationGroup = prLayoutDecorationStyleIntern(tree, &decoration);
                prLayoutTreeFrameSetStyle(tree, tab, prLayoutStyleIntern(tree, &record));
            }
            PrKitPlace tabPlace{};
            const bool currentAI = i == model.active() && model.tab(i).kind == TabKind::AI;
            const bool offersAccounts = std::find(view.windowApp.panels.begin(), view.windowApp.panels.end(),
                                                  Panel::Account) != view.windowApp.panels.end();
            const auto current = !view.windowApp.centeredAccounts && currentAI && offersAccounts && view.accountCaption ? view.accountCaption() : std::string();
            tabPlace.height = prKitPx((current.empty() ? std::min(look.tabs.tabHeight, look.tabs.height) : look.tabs.height) * k());
            tabPlace.align = PrKitAlign_Center;
            tabPlace.margin.left = (i == 0 ? look.tabs.leading : look.tabs.gap) * k();
            const auto titleText = i < view.windowApp.surface.tabTitles.size() ?
                view.windowApp.surface.tabTitles[i] : label(model.tab(i));
            if (current.empty() && look.tabs.reserveWidth) {
                const auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, tab));
                const auto box = prLayoutBoxStyleRecord(tree, record.groups[PrLayoutGroup_Box]);
                const auto inset = [&](PrRectSide side) {
                    return prLayoutUnitToPx(prLayoutWordPxValue(box.padding[side].bits) + box.border[side]);
                };
                tabPlace.width = prKitPx(tabAdvance(i, titleText) + inset(PrRectSide_Left) + inset(PrRectSide_Right));
            }
            prKitPlace(kit, tab, tabPlace);
            const auto title = current.empty() ? tab :
                prKitBox(kit, tab, {}, {.direction = PrKitDirection_Column, .align = PrKitAlign_Start});
            const auto text = label(title, titleText, textStyle(static_cast<PrKitTextRole>(active ? kSelectedTabRole : kTabRole), PrKitTone_Primary));
            auto titleStyle = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, text));
            auto& colors = titleStyle.groups[PrLayoutGroup_Color];
            auto ink = prLayoutColorStyleRecord(tree, colors);
            ink.color = prColor16FromColor(rgb(active ? look.tabs.selectedInk : look.tabs.ink));
            colors = prLayoutColorStyleIntern(tree, &ink);
            prLayoutTreeFrameSetStyle(tree, text, prLayoutStyleIntern(tree, &titleStyle));
            if (!current.empty()) accountDropdown(title, current);
            if (active && model.tab(i).kind == TabKind::AI) {
                view.menuAnchor = tab;          // a press here opens the account menu (routeAccountMenu)
                view.menuAnchorText = text;
            }
            view.tabNodes[i] = tab;
        }
        // A click on a tab, or Left or Right on a focused one, selects in the kit: made through the model's select_tab.
        // Right on the last tab selects kNoTab, which selects nothing: the next round puts the selection back.
        const uint64_t now = prKitTabsSelected(tabs);
        if (now != selected && now != kNoTab)
            select(first + prKitTabsIndex(tabs, now));
    }

    // Prax keeps click selection independent of drag admission. Every contiguous group has its own strip. A fork
    // that permits transfers uses the kit's cross-strip drop event, naming the source instance and destination slot.
    void strips(PrLayoutNode window) {
        view.tabWidths.resize(model.tab_count());
        view.tabNodes.assign(model.tab_count(), kPrLayoutNodeNone);
        view.accountAnchor = kPrLayoutNodeNone;
        view.accountMenu = kPrLayoutNodeNone;
        view.accountRows.clear();
        view.menuAnchor = view.menuAnchorText = kPrLayoutNodeNone;
        view.headerTabs = kPrLayoutNodeNone;
        view.headerHeight = 0;
        if (!view.windowApp.surface.showTabs) return;
        const bool exchanges = std::any_of(view.windowApp.groups.begin(), view.windowApp.groups.end(),
            [](const TabGroup& group) { return !group.acceptsFrom.empty(); });
        if (const PrUiPayload* dragging = prUiDragging(ui)) view.draggingTab = dragging->value;
        const bool many = model.tab_count() && model.tab(0).group != model.tab(model.tab_count() - 1).group;
        const bool clock = !view.windowApp.surface.clockText.empty();
        const float height = look.tabs.height * k() + look.ui.strip_border_height;
        const auto notch = view.headerNotch;
        const bool obstructed = view.windowApp.surface.flags.notch_aware && notch.right > notch.left &&
            notch.bottom > 0 && notch.top < height && notch.left < view.extent.x && notch.right > 0;
        const float leftWidth = std::clamp(notch.left - look.ui.notch_margin * k(), 0.f, view.extent.x);
        const float rightStart = std::clamp(notch.right + look.ui.notch_margin * k(), 0.f, view.extent.x);
        const bool lanes = obstructed && leftWidth >= look.ui.notch_min_lane_width * k() &&
            view.extent.x - rightStart >= look.ui.notch_min_lane_width * k();
        if (model.tab_count() || clock || obstructed)
            view.headerHeight = (lanes ? std::max(height, notch.bottom) : height + (obstructed ? notch.bottom : 0)) +
                look.ui.tab_divider_height;
        // When a narrow view or large text leaves too little room beside the notch, the header moves below it.
        if (obstructed && !lanes) {
            const auto band = element(window, Part::StripRow);
            prKitPlace(kit, band, {.height = prKitPx(notch.bottom), .flex = PrKitFlex_Fixed});
        }
        const PrLayoutNode row = many || clock || obstructed ? element(window, Part::StripRow) : window;
        if (lanes)
            prKitPlace(kit, row, {.height = prKitPx(std::max(height, notch.bottom)), .flex = PrKitFlex_Fixed});
        PrLayoutNode tabsParent = row;
        if (obstructed) {
            PrKitBoxStyle lane{};
            lane.direction = PrKitDirection_Row;
            lane.overflow = PrKitOverflow_Scroll;
            lane.place.minWidth = prKitPx(0);
            lane.place.flex = static_cast<PrKitFlex>(lanes ? PrKitFlex_Fixed : PrKitFlex_Fill);
            if (lanes) lane.place.width = prKitPx(leftWidth);
            tabsParent = prKitBox(kit, row, {.key = prHashString(sv("header-tabs"), 0)}, lane);
            view.headerTabs = tabsParent;
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, tabsParent));
            auto box = prLayoutBoxStyleRecord(tree, record.groups[PrLayoutGroup_Box]);
            box.overflowY = PrLayoutOverflow_Hidden;
            record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
            prLayoutTreeFrameSetStyle(tree, tabsParent, prLayoutStyleIntern(tree, &record));
        }
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
            if (many && extensions) caption(tabsParent, group, textStyle(PrKitTextRole_BodySmall, kStatusTone));
            strip(tabsParent, tabs.get(), first, end, group, exchanges,
                  static_cast<PrKitFlex>(!obstructed && (many || clock) && end == model.tab_count() ? PrKitFlex_Fill : PrKitFlex_Fixed));
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
        if (obstructed && model.tab_count()) {
            const auto selected = kitId(model.tab(model.active()));
            const float width = (lanes ? leftWidth : view.extent.x) / k();
            if (selected != view.revealedHeaderTab || width != view.revealedHeaderWidth)
                prUiReveal(ui, view.tabNodes[model.active()]);
            view.revealedHeaderTab = selected;
            view.revealedHeaderWidth = width;
        } else {
            view.revealedHeaderTab = 0;
            view.revealedHeaderWidth = 0;
        }
        PrLayoutNode clockParent = row;
        if (lanes) {
            prKitBox(kit, row, {}, {.place = {.width = prKitPx(rightStart - leftWidth), .flex = PrKitFlex_Fixed}});
            clockParent = prKitBox(kit, row, {}, {.direction = PrKitDirection_Row, .justify = PrKitJustify_End,
                .overflow = PrKitOverflow_Clip, .place = {.minWidth = prKitPx(0), .flex = PrKitFlex_Fill}});
        }
        if (clock) {
            if (!obstructed && model.tab_count() == 0)
                prKitPlace(kit, prKitBox(kit, row, {}, {}), {.flex = PrKitFlex_Fill});
            auto clockStyle = textStyle(PrKitTextRole_Meta, kStatusTone);
            clockStyle.wrap = PrKitWrap_Clip;
            const auto node = label(clockParent, view.windowApp.surface.clockText, clockStyle);
            ink(node, look.colors.clock_ink);
            PrKitPlace place{};
            place.flex = PrKitFlex_Fixed;
            place.align = PrKitAlign_Center;
            place.margin.left = look.ui.clock_margin_left * k();
            place.margin.right = look.ui.clock_margin_right * k();
            place.margin.top = look.ui.clock_offset_y * k();
            prKitPlace(kit, node, place);
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, node));
            auto font = prLayoutFontStyleRecord(tree, record.groups[PrLayoutGroup_Font]);
            auto face = prTextFontStyle(font.font);
            face.size = look.ui.clock_size * k();
            if (!view.clockFont || prTextFontStyle(view.clockFont.get()).size != face.size) {
                const PrTextFontDesc desc{.set = kit->theme.fonts, .families = &kit->theme.ui,
                    .familyCount = 1, .style = face, .locale = prTextFontLocale(font.font)};
                view.clockFont.reset(prTextFontCreate(&desc));
            }
            font.font = view.clockFont.get();
            record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &font);
            prLayoutTreeFrameSetStyle(tree, node, prLayoutStyleIntern(tree, &record));
        }
        if (model.tab_count() > 0 || clock || obstructed) element(window, Part::TabDivider);
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

    // An AI's tab shows the AI's name; the sprint tab "sprint"; a terminal's its label (Terminals::title: the
    // machine its ssh reaches, or the title its shell set). An untitled terminal starts blank.
    std::string label(const Tab& tab) {
        if (tab.kind == TabKind::AI || tab.kind == TabKind::Extension)
            return std::string(tab.displayName());
        if (tab.kind == TabKind::Sprint)
            return look.words.dashboard_title;
        return view.terminals != nullptr ? view.terminals->title(tab.id) : std::string();
    }

    // Each AI keeps its scroll while another shows, wherever its tab stands: an AI that showed its newest entry
    // (and one never shown) comes back at its newest entry, and one scrolled up comes back at the same offset from the
    // top, where the same entries show, since entries only arrive at the end. Returns false if the transcript should not
    // follow its end this round: it put back a view scrolled up, and the last layout was another AI's.
    bool keepScroll(PrLayoutNode scroller, std::size_t id) {
        if (view.shown == id)
            return true;
        leaveTranscript();
        view.shown = id;
        if (view.scrolls[id] == View::kAtEnd) {
            // An offset past the end, which layout clamps to the end of the content this round makes, as prUiFollowEnd
            // sets it (ui.cpp 2488-2493): prUiFollowEnd itself reads the last layout, which was another AI's.
            prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), PrLayoutUnit{INT32_MAX}});
            return true;
        }
        prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(view.scrolls[id])});
        return false;
    }

    // Keeps the scroll of the AI the transcript showed last round (by Tab::id; the AIs' ids are 0 to the number
    // of AIs less one, below tab_count()).
    void leaveTranscript() {
        view.scrolls.resize(model.tab_count(), View::kAtEnd);
        PrLayoutScrollInfo info{};
        if (view.shown < view.scrolls.size() && view.transcript != kPrLayoutNodeNone &&
                prLayoutNodeScrollInfo(tree, view.transcript, &info) && info.scrollContainer)
            view.scrolls[view.shown] =
                info.offset.y.raw < info.range.bottom.raw ? prLayoutUnitToPx(info.offset.y) : View::kAtEnd;
    }

    // The prompt is not declared this round. A focus left on it goes with it: what shows in its place may reuse its
    // parent's node (a terminal tab's view is the first element under `window`, as an AI's panels, which hold the
    // prompt, were), so a focus kept on the gone prompt reads as within the new view (prUiIsFocusWithin), the terminal
    // asks for none, and what is typed next goes nowhere.
    void promptGone() {
        if (view.prompt != kPrLayoutNodeNone && prUiFocus(ui) == view.prompt)
            prUiSetFocus(ui, kPrLayoutNodeNone);
        view.prompted = ~std::size_t(0);
        view.prompt = kPrLayoutNodeNone;
        view.promptBox = kPrLayoutNodeNone;
    }

    // A terminal tab: its terminal fills the window's width and the height below the strip, with no prompt. The
    // transcript and the prompt are not declared, so the AI shown before keeps its scroll, and its draft comes back
    // with the caret at its end.
    void terminal(PrLayoutNode window, const Tab& tab) {
        if (view.shown != ~std::size_t(0))
            leaveTranscript();
        view.shown = ~std::size_t(0);
        view.transcript = kPrLayoutNodeNone;
        promptGone();
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
    // is no page to show, the line that says why. With no AIs, a caption beneath the board gives the reason.
    // Like a terminal tab, it leaves the AI shown before with its scroll and its draft.
    void sprint(PrLayoutNode window) {
        if (view.shown != ~std::size_t(0))
            leaveTranscript();
        view.shown = ~std::size_t(0);
        view.transcript = kPrLayoutNodeNone;
        promptGone();
        // The box takes the room the strip leaves; the page is laid out at the box's extent from the last
        // layout (the strip's height beneath the window's in the first).
        PrLayoutNode box = element(window, Part::BoardBox);
        PrVec2 extent = boardExtent();
        if (view.boardBox != kPrLayoutNodeNone) {
            PrRect last = prLayoutRectToPx(prLayoutNodeBounds(tree, view.boardBox, PrLayoutCoordinates_Layout));
            if (last.right - last.left >= 1 && last.bottom - last.top >= 1)
                extent = PrVec2{last.right - last.left, last.bottom - last.top};
        }
        view.boardBox = box;
        std::string problem = look.words.dashboard_unavailable;
        if (view.board == nullptr || !view.board->show(box, extent, &problem)) {
            PrLayoutNode column = element(box, Part::Column);
            PrLayoutNode centered = element(column, Part::Centered);
            caption(centered, problem, textStyle(PrKitTextRole_Body, kNoticeTone, PrKitTextAlign_Center));
            // A dashboard waits on the server, so the link's own line shows under it while there is one: connecting,
            // lost, or why it failed (a host key that changed, a key the server did not take), never hidden behind
            // the wait.
            if (!view.status.empty())
                caption(centered, view.status, textStyle(PrKitTextRole_BodySmall, kStatusTone, PrKitTextAlign_Center));
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

    // The AI's transcript as laid out (TranscriptItems), with the entries that arrived since grouped into items:
    // an entry each, and a run of tool lines one item.
    TranscriptItems& itemsOf(const Tab& tab) {
        if (view.items.size() <= tab.id)
            view.items.resize(tab.id + 1);
        TranscriptItems& items = view.items[tab.id];
        if (items.generation != tab.transcriptGeneration) {
            items = {};
            items.generation = tab.transcriptGeneration;
        }
        const std::vector<Entry>& entries = tab.transcript;
        for (std::size_t i = items.entries; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if ((e.kind == EntryKind::Tool && (!look.transcriptStyle.showTools ||
                    e.text.starts_with("Agent:") || e.text.starts_with("SendMessage:") ||
                    e.text.starts_with("mcp__nexus__nexus_bus_send:") ||
                    e.text.starts_with("nexus_bus_send:"))) ||
                    (e.kind == EntryKind::Notice && !look.transcriptStyle.showsNotice(e.text)))
                continue;
            uint32_t lines = 1;
            for (char c : e.text)
                lines += c == '\n' ? 1 : 0;
            const bool joins = e.kind == EntryKind::Tool && i != 0 && entries[i - 1].kind == EntryKind::Tool &&
                !items.first.empty() && items.first.back() + items.count.back() == i;
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
                    items.height[item] = r.bottom - r.top + look.ui.entry_gap * k();
            }
        }
        items.declared.clear();
        if (items.width < 1)
            items.width = view.extent.x * look.prompt.width / 100;
    }

    // An item's height with the gap after it: as last laid out, or an estimate from its text until it is.
    float heightOf(const TranscriptItems& items, std::size_t i, const Tab& tab) const {
        if (items.height[i] > 0)
            return items.height[i];
        const float s = k();
        const float gap = look.ui.entry_gap * s;
        const EntryKind kind = tab.transcript[items.first[i]].kind;
        if (kind == EntryKind::Tool)
            return float(items.count[i]) * look.ui.tool_pitch * s + (float(items.count[i] - 1) * look.transcriptStyle.toolGap + 2 * look.transcriptStyle.captionPaddingY) * s + gap;
        // Characters of the body face are about half its size wide.
        const float perChar = 0.5f * look.ui.body_size * s;
        const float maxPercent = kind == EntryKind::Person ? look.ui.person_bubble_max_percent : look.ui.bubble_max_percent;
        const float width = std::max(items.width * maxPercent / 100 - 2 * look.ui.bubble_padding_x * s, 1.0f);
        const float pad = 2 * look.ui.bubble_padding_y * s;
        const float rows = float(items.lines[i]) + std::floor(float(items.chars[i]) * perChar / width);
        return rows * look.ui.line_pitch * s + pad + gap;
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
            float y = look.ui.column_inset * k();
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
            spacer(column, prHashMixKeys(base | 1, 0), above - look.ui.entry_gap * k());
        for (std::size_t i = first; i < end; ++i)
            items.declared.emplace_back(uint32_t(i), declareItem(column, tab, items, i, base));
        if (end != n)
            spacer(column, prHashMixKeys(base | 1, 1), below - look.ui.entry_gap * k());
    }

    void spacer(PrLayoutNode column, PrUiKey key, float height) {
        PrLayoutNode node = element(column, Part::Spacer, 0, key);
        PrKitPlace place{};
        place.height = prKitPx(std::max(height, 0.0f));
        prKitPlace(kit, node, place);
    }

    void glyphLight(PrLayoutNode leaf, size_t start, const View::GlyphTrail& trail, unsigned color) {
        const auto end = start + prLayoutNodeText(tree, leaf).size;
        const auto& style = look.transcriptStyle;
        const double peak = std::clamp(prUiDisplay(ui).headroom, 1.f, style.glyphPeak);
        if (peak <= 1 || prUiUserPreferences(ui).reducedMotion) return;
        std::vector<PrLayoutHighlight> highlights;
        for (const auto& range : trail.ranges) {
            if (range.start >= end || range.end <= start) continue;
            const double t = std::clamp(1 - (view.time - range.birth) / style.glyphSeconds, 0.0, 1.0);
            if (t == 0) continue;
            const double gain = std::round(128 * (1 + (peak - 1) * t * t)) / 128;
            PrLayoutHighlight h{};
            h.start = uint32_t(std::max(range.start, start) - start);
            h.end = uint32_t(std::min(range.end, end) - start);
            h.color = prColor16FromColor(linearGain(rgb(color), gain));
            highlights.push_back(h);
        }
        if (highlights.empty()) return;
        const size_t n = highlights.size();
        highlights.resize(n + prLayoutNodeHighlights(tree, leaf, nullptr, 0));
        if (highlights.size() > n)
            prLayoutNodeHighlights(tree, leaf, highlights.data() + n, uint32_t(highlights.size() - n));
        // A reconciled leaf can still hold last frame's light. Keep only selection/IME decorations.
        highlights.erase(std::remove_if(highlights.begin() + n, highlights.end(), [](const auto& h) {
            return h.background.a == 0 && h.underline.a == 0;
        }), highlights.end());
        prLayoutTreeFrameSetHighlights(tree, leaf, highlights.data(), uint32_t(highlights.size()));
        view.energyAnimating = true;
    }

    void lightNewText(View::GlyphTrail& trail, std::string_view text, size_t start, size_t end) {
        if (start >= end || prUiDisplay(ui).headroom <= 1 || prUiUserPreferences(ui).reducedMotion ||
                look.transcriptStyle.glyphPeak <= 1) return;
        // Bound the work for a large paste or stream packet, keeping grapheme clusters intact.
        const size_t stride = (end - start + 15) / 16;
        const double stagger = look.transcriptStyle.glyphStagger;
        for (size_t at = start; at < end;) {
            const size_t first = at;
            do { at = prUnicodeGraphemeNext(sv(text), at); } while (at < end && at - first < stride);
            const double age = std::min(stagger, double(first - start) * look.ui.glyph_birth_spacing);
            trail.ranges.push_back({first, at, view.time + age});
        }
    }

    void expire(View::GlyphTrail& trail) {
        std::erase_if(trail.ranges, [&](const auto& range) {
            return view.time - range.birth >= look.transcriptStyle.glyphSeconds;
        });
    }

    void message(PrLayoutNode parent, std::string_view source, EntryKind speaker, double gain = 1,
            const View::GlyphTrail* trail = nullptr) {
        const auto& style = look.transcriptStyle;
        const auto tone = speaker == EntryKind::Person ? kPersonTone : speaker == EntryKind::Notice ? kNoticeTone : kChatTone;
        std::optional<unsigned> sourceInk;
        if (speaker == EntryKind::Notice) {
            sourceInk = style.noticeInk;
        } else if (speaker == EntryKind::Bus) {
            sourceInk = style.busInk;
        } else if (speaker == EntryKind::Person) {
            const auto tagged = style.messageInks.find(source);
            if (tagged != style.messageInks.end()) sourceInk = tagged->second;
        }
        const bool bold = tone == kPersonTone && look.transcriptStyle.personBold;
        const auto body = textStyle(static_cast<PrKitTextRole>(bold ? PrKitTextRole_Strong : PrKitTextRole_Body), tone);
        const auto plainLabel = [&](PrLayoutNode node) {
            if (trail) textNodes(tree, node, [&](auto leaf) { glyphLight(leaf, 0, *trail, look.transcriptStyle.ink); });
            if (gain != 1 || sourceInk) ink(node, sourceInk.value_or(style.personInk), false, gain);
            return node;
        };
        const auto select = [&](PrLayoutNode p) { view.selection.paragraph(p, prColor16FromColor(faded(rgb(look.prompt.selection), look.ui.transcript_selection_opacity))); };
        if (!look.transcriptStyle.markdown) {
            select(plainLabel(label(parent, source, body)));
            return;
        }
        const auto& paragraphs = view.markdown.get(source, speaker != EntryKind::AI);
        if (paragraphs.size() == 1 && paragraphs.front().runs.size() == 1 && paragraphs.front().runs.front().href.empty() && !paragraphs.front().depth &&
                paragraphs.front().runs.front().role == PrKitTextRole_Body && paragraphs.front().runs.front().text == source) {
            select(plainLabel(label(parent, source, body)));
            return;
        }
        const auto column = element(parent, Part::Paragraphs);
        PrLayoutNode list = kPrLayoutNodeNone;
        size_t sourceOffset = 0;
        for (const auto& paragraph : paragraphs) {
            const auto em = kit->theme.types[PrKitTextRole_Body].size;
            auto row = column;
            if (paragraph.depth) {
                PrKitBoxStyle layout{.direction = PrKitDirection_Row, .align = PrKitAlign_Start};
                layout.padding.left = em * (look.ui.list_column_em * paragraph.ancestorColumns +
                    (paragraph.depth - 1) * look.transcriptStyle.listGap);
                if (list == kPrLayoutNodeNone)
                    list = prKitBox(kit, column, {}, {.gap = look.ui.line_pitch * k() * look.transcriptStyle.listItemGapLines});
                row = prKitBox(kit, list, {}, layout);
                const auto marker = plainLabel(label(row, paragraph.marker, body));
                PrKitPlace gutter{};
                gutter.width = prKitPx(em * (look.ui.list_column_em * paragraph.markerColumns + look.transcriptStyle.listGap));
                gutter.flex = PrKitFlex_Fixed;
                prKitPlace(kit, marker, gutter);
            } else {
                list = kPrLayoutNodeNone;
            }
            const auto text = prKitText(kit, row, {}, body);
            const auto plain = sourceInk.value_or(tone == kPersonTone ? style.personInk : style.ink);
            for (const auto& run : paragraph.runs) {
                auto runStyle = textStyle(static_cast<PrKitTextRole>(bold && run.role == PrKitTextRole_Body ? PrKitTextRole_Strong : run.role), tone);
                // Keep Markdown in the paragraph's line flow. The kit's default monospace role is nonwrapping.
                runStyle.wrap = PrKitWrap_Wrap;
                const auto span = prKitText(kit, text, {}, runStyle);
                const auto ordinary = run.inlineCode ? style.codeInk.value_or(plain) : plain;
                const auto color = speaker == EntryKind::Bus ? style.busInk :
                    run.href.empty() ? ordinary : style.linkInk.value_or(look.tabs.selectedFill);
                ink(span, color, true, gain);
                if (run.inlineCode) {
                    auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, span));
                    auto& fontGroup = record.groups[PrLayoutGroup_Font];
                    auto font = prLayoutFontStyleRecord(tree, fontGroup);
                    auto face = prTextFontStyle(font.font);
                    face.size += style.codeSizeOffset * k();
                    if (!view.inlineFont || prTextFontStyle(view.inlineFont.get()).size != face.size ||
                            prTextFontSet(view.inlineFont.get()) != kit->theme.fonts) {
                        const PrTextFontDesc desc{.set = kit->theme.fonts, .families = &kit->theme.mono,
                            .familyCount = 1, .style = face, .locale = prTextFontLocale(font.font)};
                        view.inlineFont.reset(prTextFontCreate(&desc));
                    }
                    font.font = view.inlineFont.get();
                    font.lineHeight = prLayoutLineHeightPx(face.size * look.ui.code_line_height);
                    fontGroup = prLayoutFontStyleIntern(tree, &font);
                    auto& boxGroup = record.groups[PrLayoutGroup_Box];
                    auto box = prLayoutBoxStyleRecord(tree, boxGroup);
                    padding(box, 0, style.codePadding * k(), 0, style.codePadding * k());
                    boxGroup = prLayoutBoxStyleIntern(tree, &box);
                    if (style.codeBackground) {
                        auto& group = record.groups[PrLayoutGroup_Decoration];
                        auto decoration = prLayoutDecorationStyleRecord(tree, group);
                        decoration.background = prColor16FromColor(rgb(style.codeFill));
                        radius(decoration, style.codeRadius * k());
                        group = prLayoutDecorationStyleIntern(tree, &decoration);
                    }
                    prLayoutTreeFrameSetStyle(tree, span, prLayoutStyleIntern(tree, &record));
                }
                const auto leaf = prLayoutTreeFrameText(tree, span, 0, sv(run.text));
                if (trail) {
                    const auto offset = source.find(run.text, sourceOffset);
                    if (offset != source.npos) {
                        glyphLight(leaf, offset, *trail, color);
                        sourceOffset = offset + run.text.size();
                    }
                }
                if (!run.href.empty()) view.selection.link(leaf, run.href);
            }
            if (paragraph.depth) {
                PrKitPlace content{};
                content.minWidth = prKitPx(0);
                content.flex = PrKitFlex_Fill;
                prKitPlace(kit, text, content);
            }
            select(paragraph.depth ? row : text);
        }
    }

    void switchLabel(PrLayoutNode bubble, const Entry& entry) {
        const std::string_view text = entry.text;
        const auto start = kAccountSwitchPrefix.size();
        const std::size_t end = entry.accountEmailEnd;
        const auto body = textStyle(PrKitTextRole_Body, kStatusTone);
        if (!text.starts_with(kAccountSwitchPrefix) || end <= start || end > text.size() ||
                (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)) {
            label(bubble, text, body);
            return;
        }
        // Keep the address in one wrapping paragraph. A local inline color leaves the Kit theme's control colors alone.
        const auto paragraph = prKitText(kit, bubble, {}, body);
        prLayoutTreeFrameText(tree, paragraph, 0, sv(text.substr(0, start)));
        auto run = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, paragraph));
        run.groups[PrLayoutGroup_Box] = prLayoutStyleDefault().groups[PrLayoutGroup_Box];
        auto ink = prLayoutColorStyleRecord(tree, run.groups[PrLayoutGroup_Color]);
        ink.color = prColor16FromColor(rgb(look.transcriptStyle.switchEmailInk));
        run.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &ink);
        const auto address = prLayoutTreeFrameElement(tree, paragraph, 0, prLayoutStyleIntern(tree, &run));
        prLayoutTreeFrameText(tree, address, 0, sv(text.substr(start, end - start)));
        prLayoutTreeFrameText(tree, paragraph, 0, sv(text.substr(end)));
    }

    float scannerMargin(const Tab& tab, float panelInset) const {
        const float iconSpace = showsPromptPlus(tab) ? look.ui.plus_size + look.prompt.gap : 0;
        return 1 + (look.prompt.paddingX + iconSpace - panelInset + look.transcriptStyle.scannerOffset) * k();
    }

    PrLayoutNode assistantMessage(PrLayoutNode row, const Tab& tab) {
        const auto message = element(row, Part::AssistantMessage);
        auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, message));
        auto& group = record.groups[PrLayoutGroup_Box];
        auto box = prLayoutBoxStyleRecord(tree, group);
        box.margin[PrRectSide_Left] = px(scannerMargin(tab, view.conversation ? look.ui.bubble_padding_x : 0));
        group = prLayoutBoxStyleIntern(tree, &box);
        prLayoutTreeFrameSetStyle(tree, message, prLayoutStyleIntern(tree, &record));
        return message;
    }

    // One item, keyed by its tab and place, so that it keeps its node and its set text as the items in sight change.
    PrLayoutNode declareItem(PrLayoutNode column, const Tab& tab, const TranscriptItems& items, std::size_t i,
            uint64_t base) {
        const PrUiKey key = prHashMixKeys(base | 2, items.first[i]);
        const Entry& e = tab.transcript[items.first[i]];
        switch (e.kind) {
            case EntryKind::Bus: {
                const auto row = element(column, Part::ReplyRow, 0, key);
                const auto bubble = element(row, Part::Bubble);
                std::string_view body = e.text;
                const auto separator = body.find('\n');
                if (separator != body.npos) {
                    const auto sender = label(bubble, body.substr(0, separator), textStyle(PrKitTextRole_Meta, kChatTone));
                    ink(sender, look.transcriptStyle.busInk);
                    view.selection.paragraph(sender, prColor16FromColor(faded(rgb(look.prompt.selection), look.ui.transcript_selection_opacity)));
                    body.remove_prefix(separator + 1);
                }
                message(bubble, body, e.kind);
                return row;
            }
            case EntryKind::Person:
            case EntryKind::AI:
            case EntryKind::Notice: {
                PrLayoutNode entry = element(column, e.kind == EntryKind::Person ? Part::Entry : Part::ReplyRow, 0, key);
                PrLayoutNode bubble = e.kind == EntryKind::AI ? assistantMessage(entry, tab) :
                    e.kind == EntryKind::Person ? element(entry, Part::PersonBubble) : element(entry, Part::CaptionBubble);
                if (e.kind == EntryKind::Notice && e.accountEmailEnd) {
                    switchLabel(bubble, e);
                } else if (e.kind == EntryKind::Notice) {
                    message(bubble, e.text, EntryKind::Notice);
                } else {
                    double gain = 1;
                    if (e.kind == EntryKind::Person && !prUiUserPreferences(ui).reducedMotion) {
                        const auto& style = look.transcriptStyle;
                        const double peak = std::clamp(prUiDisplay(ui).headroom, 1.f, style.personFlashPeak);
                        for (const auto& flash : view.sentFlashes) {
                            if (flash.tab != tab.id || flash.generation != tab.transcriptGeneration ||
                                items.first[i] < flash.first || flash.text != e.text) continue;
                            const double remaining = std::clamp(1 - (view.time - flash.since) / style.personFlashSeconds, 0.0, 1.0);
                            gain = std::round(128 * (1 + (peak - 1) * remaining * remaining)) / 128;
                            view.energyAnimating |= gain > 1;
                        }
                    }
                    const auto& reply = view.replyTrail;
                    const bool justClosed = e.kind == EntryKind::AI && reply.tab == tab.id &&
                        reply.generation == tab.transcriptGeneration && items.first[i] == reply.entry && !reply.ranges.empty();
                    const auto compact = e.kind == EntryKind::Person ? view.attachmentLabels.find({tab.id, e.text}) : view.attachmentLabels.end();
                    message(bubble, compact == view.attachmentLabels.end() ? e.text : compact->second, e.kind, gain, justClosed ? &reply : nullptr);
                }
                return entry;
            }
            case EntryKind::Tool:
                break;
        }
        PrLayoutNode tools = element(column, Part::Tools, 0, key);
        if (look.transcriptStyle.toolChevron) {
            const auto& settings = look.transcriptStyle;
            const auto chevron = prKitPathIcon(kit, tools,
                {.key = prHashString(sv("tool-chevron"), 0), .path = prKitIconPath(kit, PrStudioIconKind_ChevronRight),
                 .color = rgb(settings.toolChevronInk), .stroked = true},
                {.size = settings.toolChevronSize * k(), .tone = kStatusTone, ._pad = {}});
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, chevron));
            auto box = prLayoutBoxStyleRecord(tree, record.groups[PrLayoutGroup_Box]);
            box.position = PrLayoutPosition_Absolute;
            box.inset[PrRectSide_Left] = px(settings.captionPaddingX * k());
            box.inset[PrRectSide_Top] = px(settings.captionPaddingY * k());
            record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
            prLayoutTreeFrameSetStyle(tree, chevron, prLayoutStyleIntern(tree, &record));
        }
        PrKitTextStyle line = textStyle(PrKitTextRole_Mono, kStatusTone);
        line.wrap = PrKitWrap_Clip;
        for (std::uint32_t t = 0; t < items.count[i]; ++t) {
            const std::string_view summary = tab.transcript[items.first[i] + t].text;
            const auto separator = summary.find(": ");
            const auto& prompts = look.transcriptStyle.toolPrompts;
            const auto tool = summary.substr(0, separator);
            const auto prompt = prompts.find(tool);
            const bool customPrompt = separator != summary.npos && prompt != prompts.end();
            const bool readPath = tool == "Read" && separator != summary.npos;
            auto shown = summary;
            if (customPrompt) shown.remove_prefix(separator + 2);
            const auto node = label(tools, customPrompt ? prompt->second + std::string(shown) : std::string(shown), line);
            const auto& style = look.transcriptStyle;
            const auto toolInk = style.toolInks.find(tool);
            ink(node, toolInk != style.toolInks.end() ? toolInk->second :
                readPath ? style.readPathInk.value_or(style.linkInk.value_or(look.tabs.selectedFill)) :
                style.commandInk.value_or(customPrompt ? style.ink : style.statusInk));
            view.selection.paragraph(node, prColor16FromColor(faded(rgb(look.prompt.selection), look.ui.transcript_selection_opacity)));
        }
        return tools;
    }

    void transcript(PrLayoutNode window) {
        PrLayoutNode scroller = element(window, Part::Transcript, PrUiFlag_Scrollable);
        PrLayoutNode column = element(scroller, look.transcriptStyle.conversationPanel ? Part::PanelColumn : Part::Column);
        if (model.tab_count() == 0) {
            PrLayoutNode centered = element(column, Part::Centered);
            caption(centered, view.notice, textStyle(PrKitTextRole_Body, kStatusTone, PrKitTextAlign_Center));
            view.transcript = scroller;
            return;
        }
        const Tab& tab = model.tab(model.active());
        const std::string* growing = view.growing ? view.growing(tab.id) : nullptr;
        const bool sending = std::any_of(view.outgoing.begin(), view.outgoing.end(), [&](const auto& bubble) { return bubble.tab == tab.id; });
        if (look.transcriptStyle.conversationPanel && (sending || !tab.transcript.empty() || !tab.held.empty() || !tab.queue.empty() || (growing && !growing->empty())))
            view.conversation = column = element(column, Part::Conversation);
        const bool switched = view.shown != tab.id;
        // Text was just cut to fit this tab's prompt: the transcript goes to its end, where the line saying so shows.
        const bool toCut = (view.cutFollow && view.cutTab == tab.id) || view.followEndTab == tab.id;
        if (keepScroll(scroller, tab.id))
            prUiFollowEnd(ui, scroller);
        if (toCut) {
            // Past the end, which layout clamps to the end of the content this round makes (as keepScroll goes there).
            prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), PrLayoutUnit{INT32_MAX}});
            if (view.cutTab == tab.id) view.cutFollow = false;
            if (view.followEndTab == tab.id) view.followEndTab.reset();
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
        auto& reply = view.replyTrail;
        expire(reply);
        if (switched || reply.generation != tab.transcriptGeneration || reply.tab != tab.id) {
            reply = {};
            reply.tab = tab.id;
            reply.generation = tab.transcriptGeneration;
            reply.entry = tab.transcript.size();
            reply.bytes = growing ? growing->size() : 0;
        }
        declareItems(scroller, column, tab, items, atEnd, offset, viewport);
        // The AI's text still growing (the open entry), after the transcript, a label a paragraph: only its last
        // paragraph is set again as it grows. It joins the transcript, closed, when its block ends.
        if (growing != nullptr && !growing->empty()) {
            if (reply.entry != tab.transcript.size() || growing->size() < reply.bytes) {
                reply.entry = tab.transcript.size();
                reply.bytes = 0;
                reply.ranges.clear();
            }
            if (growing->size() > reply.bytes) {
                lightNewText(reply, *growing, reply.bytes, growing->size());
                reply.bytes = growing->size();
            }
            message(assistantMessage(element(column, Part::ReplyRow), tab), *growing, EntryKind::AI, 1, &reply);
        }
        // Keep the reason alongside the conversation if a prompt could not be sent.
        if (view.helpTab == tab.id)
            message(element(element(column, Part::ReplyRow), Part::CaptionBubble), view.helpText, EntryKind::Notice);
        if (!view.problem.empty()) {
            message(element(element(column, Part::ReplyRow), Part::CaptionBubble), view.problem, EntryKind::Notice);
        }
        // How much text was cut to fit the prompt, on the tab it was cut on, there too: the transcript went to its end
        // to show it, so it stands just above the prompt.
        if (!view.cut.empty() && view.cutTab == tab.id) {
            caption(element(column, Part::ReplyRow), view.cut, textStyle(PrKitTextRole_Body, kNoticeTone));
        }
        // The state cursor, on its own line under the last text: the busy one breathes, so frames go on while it shows.
        const CursorState state = view.cursor ? view.cursor(tab.id) : CursorState::None;
        const bool busy = state == CursorState::Busy || state == CursorState::Working;
        view.breathing = busy && idleAnimation(view.lastInput);
        if (busy || state == CursorState::Idle) {
            const bool reduced = !idleAnimation(view.lastInput);
            const unsigned count = busy ? std::max(1u, view.activeAgents ? view.activeAgents(tab.id) : 1u) : 1u;
            if (view.scannerTab != tab.id) {
                view.scannerTab = tab.id;
                view.scannerCount = count;
                view.scannerSpread = 1;
                view.scannerBusy = busy;
                view.scannerStarted = view.time;
                view.scannerChanged = busy ? view.time : -100;
            }
            if (busy != view.scannerBusy || (busy && count != view.scannerCount)) {
                if (busy && !view.scannerBusy) view.scannerStarted = view.time;
                view.scannerChanged = view.time;
                view.scannerBusy = busy;
                if (busy) {
                    view.scannerSpread = count > view.scannerCount ? float(view.scannerCount - 1) / (count - 1) : 1;
                    view.scannerCount = count;
                }
                view.scannerTime = view.time;
            }
            const double elapsed = view.time - view.scannerChanged;
            const float goal = busy || (!reduced && elapsed < look.transcriptStyle.scannerPause) ? 1.f : 0.f;
            const float dt = float(std::max(0.0, view.time - view.scannerTime));
            view.scannerTime = view.time;
            view.scannerSpread += (goal - view.scannerSpread) * (-std::expm1(-dt / look.transcriptStyle.scannerEase));
            if (reduced || std::abs(view.scannerSpread - goal) < 0.008f) view.scannerSpread = goal;
            if (!busy && view.scannerSpread == 0) view.scannerCount = 1;
            view.breathing |= !reduced && (view.scannerSpread != goal || (!busy && elapsed < look.transcriptStyle.scannerPause + look.transcriptStyle.scannerFade));
            PrKitBoxStyle strip{.direction = PrKitDirection_Row};
            strip.place.align = PrKitAlign_Start;
            // The prompt border and its configurable inset locate the first typing column.
            const float panelInset = view.conversation == column ? look.ui.bubble_padding_x : 0;
            strip.place.margin.left = scannerMargin(tab, panelInset);
            const float above = caretInset(look.prompt, look.prompt.caretOffsetY, k());
            const float below = caretInset(look.prompt, -look.prompt.caretOffsetY, k());
            const float panelPadding = view.conversation == column ? look.ui.bubble_padding_y * k() : 0;
            const bool followsText = prLayoutNodeFirstChild(tree, column) != kPrLayoutNodeNone;
            strip.place.margin.top = above - (followsText ? look.ui.entry_gap * k() : panelPadding);
            strip.place.margin.bottom = view.conversation == column ? 0 : below;
            const auto row = prKitBox(kit, column, {}, strip);
            // Pending and sent bubbles share the scanner's row. Its absolute box leaves the
            // transcript height alone, and the bubble's stacking order keeps text in front.
            const bool pending = sending || !tab.queue.empty() || !tab.held.empty();
            const bool lastIsPerson = !tab.transcript.empty() && tab.transcript.back().kind == EntryKind::Person &&
                (!growing || growing->empty()) && view.problem.empty() && (view.cut.empty() || view.cutTab != tab.id);
            if (pending || lastIsPerson) {
                auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, row));
                auto& group = record.groups[PrLayoutGroup_Box];
                auto box = prLayoutBoxStyleRecord(tree, group);
                box.position = PrLayoutPosition_Absolute;
                box.inset[PrRectSide_Left] = px(panelInset * k());
                box.inset[PrRectSide_Right] = px(panelInset * k());
                box.margin[PrRectSide_Bottom] = px(0);
                box.inset[PrRectSide_Bottom] = px(below);
                box.overflowX = PrLayoutOverflow_Hidden;
                group = prLayoutBoxStyleIntern(tree, &box);
                prLayoutTreeFrameSetStyle(tree, row, prLayoutStyleIntern(tree, &record));
            }
            const auto& settings = look.transcriptStyle;
            const float gain = std::clamp(prUiDisplay(ui).headroom, 1.0f, settings.scannerPeak);
            const double cycle = reduced ? 0.25 : ((busy ? view.time : view.scannerChanged) - view.scannerStarted) / settings.scannerSeconds;
            const double phase = cycle - std::floor(cycle);
            // Bound layout work for large fleets. The overflow label preserves the exact count.
            const float cellWidth = look.prompt.segmentSize(1).x;
            const unsigned lamps = std::min(view.scannerCount, std::max(1u, unsigned(std::max(0.f, view.extent.x * look.prompt.width / 100 - 2 * panelInset * k() - std::max(0.f, strip.place.margin.left)) /
                ((cellWidth + settings.scannerGap) * k() + 1))) / 2 + 1);
            const double sweep = sampleMotion(settings.scannerCurve, phase);
            const double head = (lamps - 1) * sweep;
            const double activity = busy ? 1.0 : (reduced ? 0.0 : std::clamp(1.0 - elapsed / settings.scannerFade, 0.0, 1.0));
            for (unsigned i = 0; i < lamps; ++i) {
                const double distance = (i - head) / look.ui.scanner_spread; // Gaussian width in lamp spacings
                const double light = lamps == 1
                    ? (reduced ? 1.0 : std::lerp(double(settings.scannerPulseFloor), 1.0, sweep))
                    : std::exp(-0.5 * distance * distance);
                const auto steps = static_cast<int16_t>(look.textSteps);
                const auto record = pr::uiStyle(ui, look, BoxRecipe{Part::ScannerSegment, steps},
                    ScannerLight{uint8_t(std::lround(light * 255)), steps, uint16_t(std::lround(gain * 64)), uint8_t(std::lround(activity * 255))});
                const auto cell = prUiElement(ui, row, 0, record, 0);
                if (i) {
                    PrKitPlace place{};
                    place.flex = PrKitFlex_Fixed;
                    place.margin.left = (view.scannerSpread * (cellWidth + settings.scannerGap) - cellWidth) * k();
                    prKitPlace(kit, cell, place);
                }
            }
            if (busy && count > lamps) caption(row, "+" + std::to_string(count - lamps), textStyle(PrKitTextRole_Meta, kStatusTone));
        } else if (state != CursorState::None) {
            prUiElement(ui, column, 0, cursorStyle(ui, look, state, 0), 0);
        }
        const auto pendingBubble = [&](const std::string& text) {
            const auto row = element(column, Part::Entry);
            const auto bubble = element(row, Part::PendingBubble);
            const auto& settings = look.transcriptStyle;
            const bool animate = idleAnimation(view.lastInput);
            const double wave = 0.5 - 0.5 * std::cos(view.time * 6.283185307179586 / settings.pendingTextSeconds);
            const double gain = animate ? 1 - settings.pendingTextPulse * wave : 1;
            view.energyAnimating |= animate && settings.pendingTextPulse > 0;
            message(bubble, text, EntryKind::Person, gain);

            if (settings.pendingOutlineWidth <= 0) return;
            const bool moving = settings.pendingDashSeconds > 0 && idleAnimation(view.lastInput);
            view.energyAnimating |= moving;
            view.pendingDashStep = moving ? unsigned(64 * std::fmod(view.time, settings.pendingDashSeconds) / settings.pendingDashSeconds) : 0;
            auto box = prLayoutBoxStyleDefault();
            box.position = PrLayoutPosition_Absolute;
            box.inset[PrRectSide_Left] = box.inset[PrRectSide_Top] = px(0);
            box.width = box.height = prLayoutLengthPercent(100);
            auto hit = prLayoutColorStyleDefault();
            hit.pointerEvents = PrLayoutPointerEvents_None;
            auto record = prLayoutStyleDefault();
            record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
            record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &hit);
            const auto border = prLayoutTreeFrameReplaced(tree, bubble, 0, prLayoutStyleIntern(tree, &record));
            PrLayoutContent content{};
            content.kind = PrLayoutContentKind_Painter;
            content.painter.context = &view;
            content.painter.paint = paintPendingOutline;
            content.revision = prHashMixKeys(prHashMixKeys(look.tabs.selectedFill, view.pendingDashStep),
                prHashMixKeys(std::hash<float>{}(settings.pendingOutlineWidth * k()), std::hash<float>{}(settings.radius * k())));
            prLayoutTreeFrameSetContent(tree, border, &content);
        };
        for (const auto& text : tab.queue) {
            const auto compact = view.attachmentLabels.find({tab.id, text});
            message(element(element(column, Part::Entry), Part::PersonBubble),
                compact == view.attachmentLabels.end() ? text : compact->second, EntryKind::Person);
        }
        for (const auto& sendingBubble : view.outgoing)
            if (sendingBubble.tab == tab.id) message(element(element(column, Part::Entry), Part::PersonBubble), sendingBubble.text, EntryKind::Person);
        if (!tab.held.empty()) pendingBubble(tab.held);
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
        float lines = std::round(prLayoutUnitToPx(box.bottom - box.top) / (look.ui.line_pitch * k()));
        return uint32_t(std::clamp(lines, 1.0f, look.prompt.maxLines));
    }

    void promptKey(std::size_t active, const char* key) {
        const Tab& tab = model.tab(active);
        if (!tab.draft.empty() || !tab.held.empty()) view.followEndTab = tab.id;
        const bool logout = tab.draft == "/logout";
        const auto first = tab.draft.find_first_not_of(" \t\r\n");
        const bool help = first != std::string::npos &&
            std::string_view(tab.draft).substr(first, tab.draft.find_last_not_of(" \t\r\n") - first + 1) == "/help";
        policy::MessagePlan plan;
        std::string why;
        const std::string aiName(tab.displayName());
        const policy::MessageState state{tab.draft, aiName, tab.model, tab.queue.size(),
            view.cursor && (view.cursor(tab.id) == CursorState::Busy || view.cursor(tab.id) == CursorState::Working), {}, {}, {}, !view.windowApp.signInAccount.name.empty()};
        if (!policy::messagePlan(*view.application, {}, true, key, state, plan, why) ||
                (plan.text && plan.text->size() > kPromptBytes)) {
            view.problem = uiFormat(look.words.prompt_failure, {{"reason", why.empty() ? look.words.prompt_limit : why}});
            return;
        }
        view.helpTab.reset();
        if (help && plan.delivery == policy::Delivery::None && !plan.notices.empty()) {
            view.helpText.clear();
            for (const auto& line : plan.notices) {
                if (!view.helpText.empty()) view.helpText += "\n";
                view.helpText += line;
            }
            view.helpTab = tab.id;
            view.cutFollow = true;
            view.cutTab = tab.id;
            model.set_draft(active, {});
            if (state.signedOut && view.accountAction) view.accountAction("dismiss");
            view.problem.clear();
            return;
        }
        if (plan.delivery == policy::Delivery::ClearLocal) {
            model.clear_transcript(active);
            model.set_draft(active, {});
            if (view.clearOpen) view.clearOpen(tab.id);
            if (view.accountAction) view.accountAction("dismiss");
            view.problem.clear();
            return;
        }
        if (plan.delivery == policy::Delivery::SignOut) {
            // With a server, /logout asks first, naming the email (accountDialog); the dialog signs out.
            if (view.logoutTarget) {
                if (openAccountDialog(view, AccountMenuView::Dialog::Logout)) model.set_draft(active, {});
                return;
            }
            if (!view.signOut)
                view.problem = look.words.connect_sign_out;
            else if (view.signOut())
                model.set_draft(active, {});
            return;
        }
        if (logout && (plan.delivery != policy::Delivery::None || plan.text)) {
            // A fork using the older send/queue policy must never turn the command into a message.
            view.problem = look.words.enable_sign_out;
            return;
        }
        if (plan.delivery == policy::Delivery::Quiet || plan.delivery == policy::Delivery::Loud) {
            if (view.setQuiet) view.setQuiet(plan.delivery == policy::Delivery::Quiet);
            model.set_draft(active, {});
            return;
        }
        if (!view.windowApp.surface.items && !logout && !view.windowApp.surface.promptCommand && view.activateSignIn && view.activateSignIn())
            return; // Keep the exact draft, even if the fork rewrites or drops empty prompts.
        if (plan.text)
            model.set_draft(active, std::move(*plan.text));
        if ((plan.delivery == policy::Delivery::Hold || plan.delivery == policy::Delivery::Release) &&
            !tab.draft.empty() && !tab.held.empty() && tab.held.size() + 1 + tab.draft.size() > kPromptBytes) {
            view.problem = look.words.combined_message_limit;
            return;
        }
        if (plan.delivery == policy::Delivery::Hold) {
            if (model.hold_draft(active)) view.chirp(view.windowApp.surface.holdChirp);
            return;
        }
        if (plan.delivery == policy::Delivery::Release) {
            model.hold_draft(active); // Command+Enter puts the current draft last.
            const auto draft = tab.draft;
            if (!tab.held.empty()) {
                model.set_draft(active, tab.held);
                if (view.submit) view.submit(active, true);
                else model.send(active);
                if (tab.draft.empty()) {
                    if (look.transcriptStyle.personFlashPeak > 1)
                        view.sentFlashes.push_back({tab.id, tab.transcript.size(), tab.transcriptGeneration, tab.held, view.time});
                    model.pop_held(active);
                }
            }
            model.set_draft(active, draft);
            return;
        }
        if (plan.delivery != policy::Delivery::None) {
            const auto sentAt = tab.transcript.size();
            const auto sent = tab.draft;
            const bool queue = plan.delivery == policy::Delivery::Queue;
            if (view.submit)
                view.submit(active, queue);
            else if (queue)
                model.queue(active);
            else
                model.send(active);
            if (!queue && !sent.empty() && sent.front() != '/' && tab.draft.empty() &&
                    look.transcriptStyle.personFlashPeak > 1 && !prUiUserPreferences(ui).reducedMotion)
                view.sentFlashes.push_back({tab.id, sentAt, tab.transcriptGeneration, sent, view.time});
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

    PrColor promptPulse(bool visible, double amplitude) {
        const bool animate = idleAnimation(view.lastInput);
        view.energyAnimating |= visible && animate;
        const double phase = std::fmod(view.time, look.prompt.caretSeconds);
        // Reserve a moving part of the cycle even in profiles with a short period and long holds.
        const double hold = std::min<double>(look.prompt.caretHold, look.prompt.caretSeconds * 0.45);
        const double rise = (look.prompt.caretSeconds - 2 * hold) / (1 + look.prompt.caretFallRatio);
        const double travelEnd = look.prompt.caretSeconds - hold;
        const double ramp = phase < rise ? phase / rise :
            std::clamp((travelEnd - phase) / (travelEnd - rise - hold), 0.0, 1.0);
        const double wave = !animate ? 1 : look.prompt.caretCurve.empty() ?
            (1 - std::cos(3.141592653589793 * ramp)) / 2 : sampleMotion(look.prompt.caretCurve, phase / look.prompt.caretSeconds);
        const double peak = std::clamp(prUiDisplay(ui).headroom, 1.f, look.prompt.caretPeak);
        const double gain = std::round(64 * (look.ui.caret_floor + (peak - look.ui.caret_floor) * (0.5 + amplitude * (wave - 0.5)))) / 64;
        return linearGain(rgb(look.prompt.caretInk), gain);
    }

    void promptCaret(PrLayoutNode parent, PrLayoutNode edit) {
        if (pickerInput || asking || prUiFocus(ui) != edit) return;
        const auto selection = prUiTextEditSelection(ui, edit);
        PrLayoutCaret caret{};
        if (selection.anchor != selection.caret || prLayoutTreeCarets(tree, &caret, 1) != 1) return;
        auto ancestor = caret.position.node;
        while (ancestor != edit && ancestor != kPrLayoutNodeNone)
            ancestor = prLayoutNodeParent(tree, ancestor);
        if (ancestor != edit) return;

        PrLayoutRect bounds{};
        if (!prLayoutNodeTextCaret(tree, caret.position.node, caret.position.offset, caret.affinity,
                PrLayoutCoordinates_Viewport, &bounds)) return;
        const auto at = prLayoutRectToPx(bounds);
        const auto field = prLayoutRectToPx(prLayoutNodeBounds(tree, edit, PrLayoutCoordinates_Viewport));
        const auto origin = prLayoutRectToPx(prLayoutNodeBounds(tree, parent, PrLayoutCoordinates_Viewport));
        prLayoutTreeFrameSetCarets(tree, nullptr, 0);
        const auto [width, height] = look.prompt.segmentSize(k());
        const float top = (at.top + at.bottom - height) / 2;
        if (at.left < field.left || at.left > field.right || at.bottom <= field.top || at.top >= field.bottom ||
                height > field.bottom - field.top) return;

        auto box = prLayoutBoxStyleDefault();
        box.position = PrLayoutPosition_Absolute;
        box.height = px(height);
        const auto& draft = model.tab(model.active()).draft;
        const bool afterText = !draft.empty() && selection.caret == draft.size() && draft.back() != '\n';
        const bool inserting = selection.caret < draft.size();
        const float ratio = std::max(1.f, prUiDisplay(ui).pixelRatio);
        const float thin = std::round(std::max(look.ui.insertion_caret_min_width, look.ui.insertion_caret_width * k()) * ratio) / ratio;
        const float drawnWidth = inserting ? std::min(width, thin) : width;
        box.width = px(drawnWidth);
        const float left = inserting ? std::round((at.left - drawnWidth / 2) * ratio) / ratio :
            at.left + (afterText ? look.prompt.caretGap * k() : 0);
        box.inset[PrRectSide_Left] = px(std::clamp(left, field.left,
            std::max(field.left, field.right - drawnWidth)) - origin.left - 1);
        const float alignedTop = std::clamp(top + look.prompt.caretOffsetY * k(), field.top, field.bottom - height);
        box.inset[PrRectSide_Top] = px(alignedTop - origin.top - 1);
        auto decoration = prLayoutDecorationStyleDefault();
        decoration.background = prColor16FromColor(promptPulse(true, 1.0));
        radius(decoration, look.prompt.caretRadius * k());
        auto hit = prLayoutColorStyleDefault();
        hit.pointerEvents = PrLayoutPointerEvents_None;
        auto record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, &decoration);
        record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &hit);
        prLayoutTreeFrameElement(tree, parent, 0, prLayoutStyleIntern(tree, &record));
    }

    // `composing`: an input method was composing in the prompt before this round's events (read in build).
    void goalBubble(PrLayoutNode footer) {
        if (!model.tab_count() || model.tab(model.active()).kind != TabKind::AI) return;
        const auto active = model.active();
        const auto goal = view.goal ? view.goal(model.tab(active).id) : std::string();
        if (!goal.empty()) {
            const auto row = element(footer, Part::Entry, 0, prHashString(sv("active-goal"), 0));
            PrKitPlace place{};
            place.margin.bottom = look.transcriptStyle.goalGap * k();
            prKitPlace(kit, row, place);
            const auto bubble = element(row, Part::PendingBubble);
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, bubble));
            auto box = prLayoutBoxStyleRecord(tree, record.groups[PrLayoutGroup_Box]);
            box.maxWidth = prLayoutLengthPercent(look.ui.bubble_max_percent);
            auto decoration = prLayoutDecorationStyleRecord(tree, record.groups[PrLayoutGroup_Decoration]);
            const auto orange = prColor16FromColor(promptPulse(true, 1.0));
            for (int side = 0; side < PrRectSide_Count; ++side) {
                box.border[side] = prLayoutUnitFromPx(look.transcriptStyle.goalOutlineWidth * k());
                decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
                decoration.borderColor[side] = orange;
            }
            box.maxHeight = px(std::max(look.transcriptStyle.goalMinHeight,
                view.extent.y * look.transcriptStyle.goalMaxHeightPercent / 100));
            box.overflowY = PrLayoutOverflow_Scroll;
            record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
            record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, &decoration);
            prLayoutTreeFrameSetStyle(tree, bubble, prLayoutStyleIntern(tree, &record));
            caption(bubble, look.transcriptStyle.goalLabel, textStyle(PrKitTextRole_Meta, kStatusTone));
            caption(bubble, goal, textStyle(PrKitTextRole_Body, kPersonTone));
        }
    }

    void prompt(PrLayoutNode footer, bool composing) {
        const auto active = model.active();
        goalBubble(footer);
        // The prompt's rounded box, holding the plus and the text area: the kit has no rounded text area and no slot
        // for an icon inside one (frameless). Enter sends and Command-Enter queues, captured on the box, so the area
        // never sees them; every other Enter (Shift-Enter among them) goes to the area, which breaks the line (pr/ui.h).
        PrLayoutNode box = element(footer, Part::Prompt, 0, prHashString(sv("prompt-box"), 0));
        view.promptBox = box;
        bool send = !pickerInput && uiKey(box, view.windowApp.surface.keys.submit);
        bool queue = !pickerInput && uiKey(box, view.windowApp.surface.keys.queue);
        // Command-V reads the clipboard through the host, which cuts the text to the room left before the area sees it,
        // and hands it on in one event.
        if (!pickerInput && view.paste && uiKey(box, view.windowApp.surface.keys.paste)) {
            view.paste();
        }
        view.promptRows = promptRows();

        if (showsPromptPlus(model.tab(active))) plus(box);
        PrKitTextArea area{};
        area.key = prHashString(PrStringView{"prompt", 6}, 0);
        area.text = sv(model.tab(active).draft);
        const PrKitTextAreaStyle promptText{PrKitTextRole(look.prompt.bold ? PrKitTextRole_Strong : PrKitTextRole_Body), PrKitTone_Secondary};
        area.styles = &promptText;
        area.styleCount = 1;
        area.rows = view.promptRows;
        area.disabled = pickerInput;
        auto& highlight = kit->theme.palette.textSelection;
        const auto savedHighlight = highlight;
        const bool focused = !pickerInput && !asking && view.prompt != kPrLayoutNodeNone && prUiFocus(ui) == view.prompt;
        const auto selected = focused ? prUiTextEditSelection(ui, view.prompt) : PrUiTextSelection{};
        highlight = faded(promptPulse(selected.anchor != selected.caret, look.ui.selection_pulse_amplitude), look.ui.selection_opacity);
        PrLayoutNode edit = prKitTextArea(kit, box, area, PrKitFieldStyle{});
        highlight = savedHighlight;
        PrKitPlace fill{};
        fill.flex = PrKitFlex_Fill;
        prKitPlace(kit, edit, fill);
        frameless(edit);
        // The area's value (its first child) is read off the last layout, so a round that builds the area for the
        // first time (a switch from a tab with no prompt, or a tab chosen before the frame) finds none. It reads the
        // area's extent then, which the round's layout changes, so the frame runs another round and shows the value
        // bold and inked.
        const auto value = prLayoutNodeFirstChild(tree, edit);
        if (value == kPrLayoutNodeNone) {
            PrVec2 extent{};
            (void)prUiExtent(ui, edit, PrUiAxisFlag_Both, &extent);
        }
        if (look.prompt.bold && value != kPrLayoutNodeNone) {
            auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, value));
            auto& group = record.groups[PrLayoutGroup_Font];
            auto font = prLayoutFontStyleRecord(tree, group);
            if (font.font) {
                auto face = prTextFontStyle(font.font);
                face.weight = look.ui.bold_weight;
                if (!view.promptFont || prTextFontStyle(view.promptFont.get()).size != face.size ||
                        prTextFontSet(view.promptFont.get()) != kit->theme.fonts) {
                    const PrTextFontDesc desc{.set = kit->theme.fonts, .families = &kit->theme.ui,
                        .familyCount = 1, .style = face, .locale = prTextFontLocale(font.font)};
                    view.promptFont.reset(prTextFontCreate(&desc));
                }
                font.font = view.promptFont.get();
                group = prLayoutFontStyleIntern(tree, &font);
                prLayoutTreeFrameSetStyle(tree, value, prLayoutStyleIntern(tree, &record));
            }
        }
        view.prompt = edit;
        // The focus is the secret dialog's while it shows (secretDialog), and the account dialog's while it shows
        // (accountDialog): Prax's modal takes focus into its content, and its Escape closes it.
        if (!pickerInput && asking == nullptr && view.accounts.dialog == AccountMenuView::Dialog::None &&
                prUiFocus(ui) != edit && !view.promptDismissed)
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
        if (value != kPrLayoutNodeNone)
            ink(value, look.prompt.ink, false, std::clamp(prUiDisplay(ui).headroom, 1.f, look.prompt.textPeak));
        promptCaret(box, edit);
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
        // An AI's draft comes back with the caret at its end.
        if (pickerInput) {
            view.prompted = ~std::size_t(0);
        } else if (view.prompted != model.tab(active).id) {
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
        dialog.key = prHashString(sv("secret"), 0);
        const std::string title = asking->account + ": " + asking->what;
        dialog.title = sv(title);
        const PrLayoutNode body = prKitDialog(kit, window, dialog, typedDialogStyle());
        if (body == kPrLayoutNodeNone) {
            // The person closed it (Escape, the close button, a press outside): the request is cancelled.
            view.secretField = kPrLayoutNodeNone;
            if (!fresh) {
                view.secrets->cancel();
            }
            return;
        }
        label(body, asking->asker.empty() ? std::string(look.words.secret_requested_by_app) : uiFormat(look.words.secret_requested_by, {{"asker", asking->asker}}),
              textStyle(PrKitTextRole_BodySmall, PrKitTone_Muted));
        if (!asking->why.empty()) {
            label(body, "“" + asking->why + "”", textStyle(PrKitTextRole_Body, PrKitTone_Primary));
        }
        const bool enter = uiKey(body, view.windowApp.surface.keys.confirm);
        PrKitTextField field{};
        field.key = prHashString(sv("secret-field"), 0);
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
        // A modal owns its keys. The signed-out page underneath also binds arrows, Enter and Escape;
        // capturing those first would move its hidden picker instead of the visible sign-in card.
        if (view.accounts.dialog != AccountMenuView::Dialog::None) return;
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

    // The lock screen (View::lockScreen): one centred card with the lock's title and message, and nothing else of the
    // window. The nodes the rounds keep for the window's other parts are cleared, so nothing (the host's routing of
    // pastes and pointers into the prompt, the selection) reaches a part that is not drawn. The board is told it is
    // hidden, and the terminals are serviced as hidden tabs, so that their shells and page keep running unseen.
    void lockScreen(PrLayoutNode parent) {
        view.prompt = kPrLayoutNodeNone;
        view.promptBox = kPrLayoutNodeNone;
        view.transcript = kPrLayoutNodeNone;
        view.conversation = kPrLayoutNodeNone;
        view.boardBox = kPrLayoutNodeNone;
        view.secretField = kPrLayoutNodeNone;
        view.energyAnimating = false;
        view.breathing = false;
        view.dialogVisible = false;
        if (view.board != nullptr) view.board->hidden();
        if (view.terminals != nullptr) {
            for (std::size_t i = model.tab_count(); i-- > 0;) {
                const Tab& tab = model.tab(i);
                if (tab.kind != TabKind::Terminal) continue;
                if (view.terminals->ended(tab.id)) {
                    view.terminals->close(tab.id);
                    model.close_tab(i);
                } else {
                    view.terminals->update(tab.id);
                }
            }
        }
        PrLayoutNode window = element(parent, Part::Window);
        PrLayoutNode box = element(window, Part::BoardBox);
        PrLayoutNode column = element(box, Part::Column);
        PrLayoutNode centered = element(column, Part::Centered);
        const auto card = prKitButton(kit, centered, {.key = prHashString(sv("lock-screen"), 0)}, {});
        prLayoutTreeFrameSetStyle(tree, card, style(ui, look, Part::MessageCard));
        auto large = textStyle(PrKitTextRole_Display, PrKitTone_Inverse, PrKitTextAlign_Center);
        large.wrap = PrKitWrap_Wrap;
        label(card, view.lockScreen->title, large);
        auto small = textStyle(PrKitTextRole_BodySmall, PrKitTone_Inverse, PrKitTextAlign_Center);
        small.wrap = PrKitWrap_Wrap;
        label(card, view.lockScreen->message, small);
        // The keys are claimed on the outermost node of the round, as build does (no node has the focus here).
        const PrLayoutNode claim = parent != kPrLayoutNodeNone ? parent : window;
        const bool enter = uiKey(claim, view.windowApp.surface.keys.confirm, false);
        if ((prKitActivated(kit, card) || enter) && view.unlock) view.unlock();
        if (uiKey(claim, view.windowApp.surface.keys.dismiss, false) && view.hide) view.hide();
    }

    // The width of `text` in the tab label's unselected face (kTabRole), the face the account menu's rows are set in.
    float menuTextWidth(std::string_view text, PrKitTextRole role = kTabRole) {
        if (!view.windowShaper && !fallbackTabShaper)
            fallbackTabShaper.reset(prTextShaperCreate(PrTextShaperBackend_Default));
        auto* shaper = view.windowShaper ? view.windowShaper : fallbackTabShaper.get();
        if (!tabLayout) tabLayout.reset(prTextLayoutCreate());
        if (!shaper || !kit->fonts[role]) return float(text.size()) * kit->theme.types[role].size * 0.6f;
        const PrTextLayoutDesc desc{.text = sv(text), .font = kit->fonts[role], .shaper = shaper};
        prTextLayoutSet(tabLayout.get(), &desc);
        return std::ceil(prTextLayoutExtent(tabLayout.get()).x);
    }

    // Sets a node's box to `at` in the viewport (position fixed), above everything the round drew before it.
    void placeFixed(PrLayoutNode node, const MenuBox& at, unsigned fill, float opacity, float corner) {
        auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, node));
        auto& boxGroup = record.groups[PrLayoutGroup_Box];
        auto box = prLayoutBoxStyleRecord(tree, boxGroup);
        box.position = PrLayoutPosition_Fixed;
        box.inset[PrRectSide_Left] = px(at.left);
        box.inset[PrRectSide_Top] = px(at.top);
        box.inset[PrRectSide_Right] = box.inset[PrRectSide_Bottom] = prLayoutLengthAuto();
        box.width = px(at.width());
        box.height = px(at.height());
        boxGroup = prLayoutBoxStyleIntern(tree, &box);
        auto& decorationGroup = record.groups[PrLayoutGroup_Decoration];
        auto decoration = prLayoutDecorationStyleRecord(tree, decorationGroup);
        decoration.background = prColor16FromColor(faded(rgb(fill), opacity));
        radius(decoration, corner);
        decorationGroup = prLayoutDecorationStyleIntern(tree, &decoration);
        prLayoutTreeFrameSetStyle(tree, node, prLayoutStyleIntern(tree, &record));
    }

    // A row's box inside the menu: its height, and its fill (the row under the finger is lit).
    void placeRow(PrLayoutNode node, float height, float left, float right, std::optional<unsigned> fill, float opacity) {
        auto record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, node));
        auto& boxGroup = record.groups[PrLayoutGroup_Box];
        auto box = prLayoutBoxStyleRecord(tree, boxGroup);
        box.position = PrLayoutPosition_Static;
        box.display = PrLayoutDisplay_Flex;
        for (auto& edge : box.inset) edge = prLayoutLengthAuto();
        box.height = px(height);
        box.minHeight = px(height);
        box.width = prLayoutLengthPercent(100);
        padding(box, 0, right, 0, left);
        boxGroup = prLayoutBoxStyleIntern(tree, &box);
        auto& flexGroup = record.groups[PrLayoutGroup_Flex];
        auto flex = prLayoutFlexStyleRecord(tree, flexGroup);
        flex.direction = PrLayoutFlexDirection_Row;
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_SpaceBetween;
        flexGroup = prLayoutFlexStyleIntern(tree, &flex);
        auto& decorationGroup = record.groups[PrLayoutGroup_Decoration];
        auto decoration = prLayoutDecorationStyleRecord(tree, decorationGroup);
        decoration.background = fill ? prColor16FromColor(faded(rgb(*fill), opacity)) : prColor16FromColor(PrColor{0, 0, 0, 0});
        decorationGroup = prLayoutDecorationStyleIntern(tree, &decoration);
        prLayoutTreeFrameSetStyle(tree, node, prLayoutStyleIntern(tree, &record));
    }

    // The account menu under the active AI's tab (account_menu.h), while the finger that opened it is down: one row per
    // signed-in account, the current one marked, then look.words.sign_in_another. Its top is just below the tab's
    // highlight and its left edge on the highlight's; each row's text starts where the tab label's text starts, in the
    // tab label's size and the unselected weight, cut with "…" and never wrapped. Nothing about the tab changes.
    void accountDropMenu(PrLayoutNode window) {
        auto& menu = view.accounts;
        menu.node = kPrLayoutNodeNone;
        menu.rowNodes.clear();
        menu.rowTexts.clear();
        if (!menu.gesture.open() || !model.tab_count() || model.tab(model.active()).id != menu.tab || menu.rows.empty())
            return;
        const auto& s = view.windowApp.surface.accountMenu;
        float widest = 0;
        const float markWidth = menuTextWidth(s.mark) + menuTextWidth(" ");
        for (const auto& row : menu.rows) widest = std::max(widest, menuTextWidth(row.signIn ? look.words.sign_in_another : row.text) + (row.current ? markWidth : 0));
        const float inset = menu.textInset >= 0 ? menu.textInset : s.paddingX;
        AccountMenuStyle placed = s;
        placed.paddingX = std::max(inset, 0.0f);
        menu.layout = layoutAccountMenu(menu.anchor, menu.rows.size(), widest, kit->theme.types[kTabRole].size,
                                        view.extent.x, placed);
        const auto layer = element(window, Part::SelectorLayer);
        placeFixed(layer, menu.layout.menu, s.fill, 1, s.radius);
        menu.node = layer;
        const auto hovered = menu.gesture.hovered();
        for (std::size_t i = 0; i < menu.rows.size(); ++i) {
            const auto& row = menu.rows[i];
            const auto node = prUiElement(ui, layer, prHashString(sv(row.signIn ? std::string("+sign-in") : row.account), 0),
                                          style(ui, look, Part::SelectorLayer), 0);
            const bool lit = hovered && *hovered == i;
            placeRow(node, menu.layout.rows[i].height(), placed.paddingX, s.paddingX,
                     lit ? std::optional<unsigned>(s.hoverFill) : std::nullopt, s.hoverOpacity);
            const float room = menu.layout.textMax - (row.current ? markWidth : 0);
            const auto shown = ellipsize(row.signIn ? look.words.sign_in_another : row.text, room, [&](std::string_view t) { return menuTextWidth(t); });
            auto appearance = textStyle(kTabRole, PrKitTone_Primary);
            appearance.wrap = PrKitWrap_Clip;
            const auto text = label(node, shown, appearance);
            ink(text, s.ink);
            if (row.current) {
                const auto mark = label(node, s.mark, appearance);
                ink(mark, s.markInk);
            }
            menu.rowNodes.push_back(node);
            menu.rowTexts.push_back(text);
        }
    }

    // The /account, /login and /logout dialogs (openAccountDialog): Prax's dialog, over the AI's page. /account lists the
    // signed-in accounts, the current one checked, and look.words.sign_in_another; /login all linked accounts; /logout asks to confirm, naming the email. Closing one changes nothing.
    void accountDialog(PrLayoutNode window) {
        auto& menu = view.accounts;
        using Dialog = AccountMenuView::Dialog;
        if (menu.dialog != Dialog::None && (!model.tab_count() || model.tab(model.active()).id != menu.dialogTab ||
                asking != nullptr))
            menu.dialog = Dialog::None;
        if (menu.dialog == Dialog::None) {
            if (menu.dialogShown) {
                prKitSetOpen(kit, window, false);
                menu.dialogShown = false;
            }
            return;
        }
        const bool fresh = !menu.dialogShown;
        if (fresh) {
            prKitSetOpen(kit, window, true);
            menu.dialogShown = true;
        }
        const std::string& title = menu.dialog == Dialog::Account ? look.words.account_title : menu.dialog == Dialog::Login ? look.words.sign_in_title : menu.dialog == Dialog::BrowserLogout ? look.words.browser_logout_title : look.words.sign_out_title;
        PrKitDialog dialog{};
        dialog.key = prHashString(sv(std::string("account-dialog/") + title), 0);
        dialog.title = sv(std::string_view(title));
        const PrLayoutNode body = menu.dialog == Dialog::Login ?
            prKitModal(kit, window, {.key = dialog.key}, typedDialogStyle().modal) :
            prKitDialog(kit, window, dialog, typedDialogStyle());
        if (body == kPrLayoutNodeNone) {
            if (!fresh) menu.dialog = Dialog::None;
            menu.dialogShown = false;
            return;
        }
        auto about = textStyle(PrKitTextRole_Body, PrKitTone_Primary);
        about.wrap = PrKitWrap_Wrap;
        const std::string name(model.tab(model.active()).displayName());
        if (menu.dialog == Dialog::BrowserLogout) {
            label(body, look.words.browser_logout_instruction, about);
            label(body, look.words.browser_logout_hint,
                  textStyle(PrKitTextRole_BodySmall, PrKitTone_Muted));
            const auto reopen = prKitButton(kit, body, {.key = prHashString(sv("browser-logout-open"), 0),
                .text = sv(look.words.browser_open)}, {});
            const auto cancel = prKitButton(kit, body, {.key = prHashString(sv("browser-logout-cancel"), 0),
                .text = sv(look.words.cancel)}, {});
            const auto confirm = prKitButton(kit, body, {.key = prHashString(sv("browser-logout-confirm"), 0),
                .text = sv(look.words.browser_logout_continue)}, {.variant = PrKitButtonVariant_Primary});
            if (prKitActivated(kit, reopen) && view.openBrowserLogout) view.openBrowserLogout();
            if (prKitActivated(kit, cancel)) {
                menu.dialog = Dialog::None;
                if (view.cancelLoginPreparation) view.cancelLoginPreparation();
            }
            if (prKitActivated(kit, confirm) && view.browserLogoutConfirmed) view.browserLogoutConfirmed();
            return;
        }
        if (menu.dialog == Dialog::Logout) {
            label(body, uiFormat(look.words.logout_confirmation, {{"ai", name}, {"account", menu.logoutShown}}), about);
            label(body, uiFormat(menu.logoutNext.empty() ? look.words.logout_needs_sign_in : look.words.logout_next_account,
                {{"ai", name}, {"account", menu.logoutNext}}), textStyle(PrKitTextRole_BodySmall, PrKitTone_Muted));
            const auto row = prKitBox(kit, body, {}, {.direction = PrKitDirection_Row, .align = PrKitAlign_Center});
            const auto cancel = prKitButton(kit, row, {.key = prHashString(sv("logout-cancel"), 0), .text = sv(look.words.cancel)}, {});
            const auto confirm = prKitButton(kit, row, {.key = prHashString(sv("logout-confirm"), 0), .text = sv(look.words.sign_out_title)},
                                             {.variant = PrKitButtonVariant_Primary});
            if (prKitActivated(kit, cancel)) menu.dialog = Dialog::None;
            if (prKitActivated(kit, confirm)) {
                menu.dialog = Dialog::None;
                if (view.logout && !view.logout() && view.problem.empty())
                    view.problem = look.words.logout_failed;
            }
            return;
        }
        if (menu.dialog == Dialog::Login) {
            const auto count = menu.dialogRows.size();
            if (count) {
                menu.loginHighlighted = std::min(menu.loginHighlighted, count - 1);
                if (uiKey(body, view.windowApp.surface.keys.previous, false))
                    menu.loginHighlighted = (menu.loginHighlighted + count - 1) % count;
                if (uiKey(body, view.windowApp.surface.keys.next, false))
                    menu.loginHighlighted = (menu.loginHighlighted + 1) % count;
            }
            std::vector<AccountItem> items{{view.windowApp.signInHeading, "", false}};
            if (!count) items.push_back({look.words.no_linked_accounts, "", false});
            for (std::size_t i = 0; i < count; ++i) {
                const auto& row = menu.dialogRows[i];
                items.push_back({row.text, "login:" + row.account, i == menu.loginHighlighted});
            }
            accountSelector(body, items, true);
            return;
        }
        if (menu.dialogRows.empty()) {
            label(body, uiFormat(look.words.no_signed_in_account, {{"ai", name}}), about);
            return;
        }
        const auto list = prKitMenu(kit, body, {}, {});
        std::optional<AccountMenuRow> chosen;
        // A row's text is cut with "…" to the dialog's room (account_menu.dialog_text_em of the menu item's text), and
        // never past the window less a margin: a long email never runs out of the dialog.
        const auto& s = view.windowApp.surface.accountMenu;
        const float room = std::max(0.0f, std::min(s.dialogTextEm * kit->theme.types[kDialogRowRole].size,
                                                   view.extent.x - 2 * look.ui.dialog_margin));
        for (const auto& row : menu.dialogRows) {
            const std::string shown = ellipsize(row.signIn ? look.words.sign_in_another : row.text, room,
                                                [&](std::string_view t) { return menuTextWidth(t, kDialogRowRole); });
            const auto item = prKitMenuItem(kit, list,
                {.key = prHashString(sv(row.signIn ? std::string("+sign-in") : row.account), 0), .text = sv(shown),
                 .icon = PrStudioIconKind_None, .checked = row.current}, {});
            PrKitPlace size{};
            size.height = prKitPx(std::max(view.windowApp.surface.accountMenu.rowHeight, kMinRowHeight));
            prKitPlace(kit, item, size);
            if (prKitActivated(kit, item)) chosen = row;
        }
        if (!chosen) return;
        menu.dialog = Dialog::None;
        if (chosen->signIn) {
            openAccountDialog(view, Dialog::Login);
        } else if (!chosen->current && view.switchAccount) {
            if (view.playChirp) view.playChirp(view.windowApp.surface.confirmChirp);
            view.switchAccount(chosen->account);
        }
    }

    void build(PrLayoutNode parent) {
        if (view.lockScreen) {
            lockScreen(parent);
            return;
        }
        if (view.pendingBoardId && view.board) {
            if (view.board->prepared()) {
                for (std::size_t i = 0; i < model.tab_count(); ++i) {
                    if (model.tab(i).id == *view.pendingBoardId) { model.select_tab(i); break; }
                }
                view.board->cancelPreparation();
                view.pendingBoardId.reset();
            } else prUiRequestFrame(ui);
        }
        PrLayoutNode window = element(parent, Part::Window);
        view.selection.begin(tree, model.tab_count() ? model.tab(model.active()).id : SIZE_MAX);
        view.energyAnimating = false;
        view.breathing = false;     // until an AI's transcript draws a busy cursor this round
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
        pickerInput = view.accountPicker.has_value() || !view.choiceKind.empty();
        captureShortcuts(keys);
        pickerInput = pickerInput || view.accountPicker.has_value() || !view.choiceKind.empty();
        serviceTerminals();
        // On a terminal tab Escape is not claimed: the terminal claims it for its shell. Command-Escape does nothing
        // there or on the sprint tab (it is captured below all the same). On the sprint tab Escape hides.
        const TabKind activeKind = model.tab_count() == 0 ? TabKind::AI : model.tab(model.active()).kind;
        const bool hides = activeKind != TabKind::Terminal;
        // While an input method composes, its Enter commits the composition and its Escape cancels it (the KeyDown comes
        // first, then CompositionEnd): read before the area takes this round's events.
        const bool composing = view.prompt != kPrLayoutNodeNone && prUiTextEditIsComposing(ui, view.prompt);
        // Escape hides the overlay (view.hide; unset in --windowed, where it does nothing). It is claimed
        // only while no tab is being dragged: a claim comes before Prax's own Escape, which cancels a drag (ui.cpp
        // routeKey), so during a drag Escape cancels it and hides nothing. While composing it is the input method's.
        // While the secret dialog shows, Escape is the dialog's (it cancels the request) and hides nothing.
        // So is it the account dialog's while that shows: Escape closes it, changes nothing and hides nothing.
        asking = view.secrets != nullptr ? view.secrets->showing() : nullptr;
        if (hides && !pickerInput && asking == nullptr && view.accounts.dialog == AccountMenuView::Dialog::None &&
                prUiDragging(ui) == nullptr &&
                uiKey(keys, view.windowApp.surface.keys.dismiss, false) && !composing && view.hide)
            view.hide();
        std::erase_if(view.sentFlashes, [&](const View::SentFlash& flash) {
            return view.time - flash.since >= look.transcriptStyle.personFlashSeconds;
        });
        const auto beforeStrip = model.active();
        strips(window);
        if (view.terminals) {
            PrVec2 content = view.extent;
            if (view.windowApp.surface.showTabs && model.tab_count())
                content.y -= look.tabs.height * k() + look.ui.strip_border_height + 2; // strip and divider
            view.terminals->startPending(content);
        }
        // A click on the sprint tab, selected already or not, takes its page back to the board (Board::home).
        if (beforeStrip == model.active() && view.windowApp.surface.showTabs && view.board && model.tab_count() && model.tab(model.active()).kind == TabKind::Sprint &&
                prUiClicked(ui, view.tabNodes[model.active()], 0))
            view.board->home();
        // the strip may have selected or moved a tab: read the active tab again
        if (model.tab_count() != 0 && model.tab(model.active()).id != view.selectedId) {
            view.selectedId = model.tab(model.active()).id;
            view.chirp(view.windowApp.surface.tabChirp);
            if (view.selected)
                view.selected();
        }
        // Cut text on its way to a prompt this round does not build has nowhere to go (the focus left the prompt, and
        // the area does not see the event): nothing more is coming.
        if (model.tab_count() == 0 || model.tab(model.active()).kind != TabKind::AI || !view.promptOpen) {
            view.cutComing = false;
        }
        page(window, composing);
        accountDropMenu(window);
        accountDialog(window);
        const bool dialog = view.accountMenu != kPrLayoutNodeNone || asking != nullptr;
        if (dialog && !view.dialogVisible) view.chirp(view.windowApp.surface.dialogChirp);
        if (!dialog) view.energyAccount.clear();
        view.dialogVisible = dialog;
        view.displayedLimit = view.windowApp.limitOffer;
        if (!view.accountPicker && view.accounts.dialog != AccountMenuView::Dialog::Login)
            view.lastHighlightedAccount.clear();
        secretDialog(window);
        serverDialog(window);
    }

    // The server dialog (server_choice.h), on iOS where nothing names the server: Prax's dialog, titled Server, saying
    // what to type, one field with the kept server or what is typed, and the reason the last Enter was not taken.
    // Enter checks it: a good address is kept and used; a bad one stays, with its reason under the field. Closing it
    // keeps the server there was; with none kept it opens again, as the window has nothing to connect to.
    void serverDialog(PrLayoutNode window) {
        ServerChoice* choice = view.serverChoice;
        if (choice == nullptr || !choice->asking() || asking != nullptr) {
            if (view.serverShown) {
                prKitSetOpen(kit, window, false);
                view.serverShown = false;
            }
            return;
        }
        const bool fresh = !view.serverShown;
        if (fresh) {
            prKitSetOpen(kit, window, true);
            view.serverShown = true;
        }
        PrKitDialog dialog{};
        dialog.key = prHashString(sv("server"), 0);
        dialog.title = sv(look.words.server_title);
        const PrLayoutNode body = prKitDialog(kit, window, dialog, typedDialogStyle());
        if (body == kPrLayoutNodeNone) {
            if (!fresh) choice->cancel();
            view.serverShown = false;
            return;
        }
        auto about = textStyle(PrKitTextRole_Body, PrKitTone_Primary);
        about.wrap = PrKitWrap_Wrap;
        label(body, look.words.server_description, about);
        const bool enter = uiKey(body, view.windowApp.surface.keys.confirm);
        PrKitTextField field{};
        field.key = prHashString(sv("server-field"), 0);
        const std::string_view typed = choice->typed();
        field.text = sv(typed);
        field.inputKind = PrAppTextInputKind_Email;   // a keyboard with @, and no capitals put in
        const PrLayoutNode edit = prKitTextField(kit, body, field, PrKitFieldStyle{});
        if (prUiFocus(ui) != edit) prUiSetFocus(ui, edit);
        PrStringView changed{};
        if (prKitTextChanged(kit, edit, &changed)) choice->type(std::string_view(changed.data, changed.size));
        if (!choice->problem().empty()) {
            auto problem = textStyle(PrKitTextRole_BodySmall, kNoticeTone);
            problem.wrap = PrKitWrap_Wrap;
            label(body, std::string(choice->problem()), problem);
        }
        if (enter) choice->submit();
    }

    std::string policyUiState(const Tab* active) {
        std::unique_ptr<PrJsonWriter, decltype(&prJsonWriterDestroy)> json(prJsonWriterCreate(), prJsonWriterDestroy);
        auto* w = json.get();
        const auto field = [&](std::string_view name, std::string_view value) {
            prJsonWriterKey(w, sv(name));
            prJsonWriterString(w, sv(value));
        };
        prJsonWriterBeginObject(w);
        prJsonWriterKey(w, sv("notch"));
        prJsonWriterBeginObject(w);
        for (const auto& [key, value] : {std::pair{"left", view.headerNotch.left}, {"top", view.headerNotch.top},
                                       {"right", view.headerNotch.right}, {"bottom", view.headerNotch.bottom}}) {
            prJsonWriterKey(w, sv(key)); prJsonWriterNumber(w, value);
        }
        prJsonWriterEndObject(w);
        if (view.calendar.valid) {
            prJsonWriterKey(w, sv("clock"));
            prJsonWriterBeginObject(w);
            const auto number = [&](const char* key, int value) {
                prJsonWriterKey(w, sv(key)); prJsonWriterNumber(w, value);
            };
            number("weekday", view.calendar.weekday); number("day", view.calendar.day);
            number("month", view.calendar.month); number("year", view.calendar.year);
            number("hour", view.calendar.hour); number("minute", view.calendar.minute); number("second", view.calendar.second);
            prJsonWriterEndObject(w);
        }
        prJsonWriterKey(w, sv("tabs"));
        prJsonWriterBeginArray(w);
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            const auto& tab = model.tab(i);
            prJsonWriterBeginObject(w);
            field("kind", tab.kind == TabKind::AI ? "ai" : tab.kind == TabKind::Sprint ? "sprint" :
                          tab.kind == TabKind::Extension ? "extension" : "terminal");
            field("label", label(tab));
            field("group", tab.group);
            field("type", tab.type);
            prJsonWriterEndObject(w);
        }
        prJsonWriterEndArray(w);
        prJsonWriterKey(w, sv("dismissed"));
        prJsonWriterBool(w, view.dismissedSignIn.has_value());
        prJsonWriterKey(w, sv("goal_active"));
        prJsonWriterBool(w, active && view.goal && !view.goal(active->id).empty());
        prJsonWriterKey(w, sv("help_visible"));
        prJsonWriterBool(w, active && view.helpTab == active->id);
        field("draft", active ? active->draft : std::string_view{});
        prJsonWriterKey(w, sv("logout"));
        prJsonWriterBool(w, active && active->draft == "/logout");
        if (active && active->kind == TabKind::AI && view.sessionCapabilities) {
            std::string modelName, effort, catalog;
            view.sessionCapabilities(modelName, effort, catalog);
            field("effort", effort);
            field("choice_kind", view.choiceKind);
            field("choice_value", view.choiceValue);
            field("model", modelName);
            std::unique_ptr<PrJson, decltype(&prJsonDestroy)> parsed(prJsonCreate(), prJsonDestroy);
            if (!catalog.empty() && prJsonParse(parsed.get(), sv(catalog), nullptr) && prJsonKind(parsed.get(), 0) == PrJsonKind_Object) {
                prJsonWriterKey(w, sv("catalog"));
                prJsonWriterValue(w, parsed.get(), 0);
            }
        }
        if (active && active->kind == TabKind::AI && view.previewAccounts) {
            auto picker = view.accountPicker ? view.accountPicker : view.previewAccounts(0);
            if (picker) {
                field("current", picker->account);
                field("highlighted", picker->options[picker->highlighted].name);
                prJsonWriterKey(w, sv("accounts"));
                prJsonWriterBeginArray(w);
                for (const auto& row : picker->options) {
                    if (row.name == sync::kAutoAccountTarget) continue;
                    prJsonWriterBeginObject(w);
                    field("name", row.name);
                    field("label", row.label);
                    prJsonWriterEndObject(w);
                }
                prJsonWriterEndArray(w);
            }
        }
        prJsonWriterEndObject(w);
        const auto bytes = prJsonWriterText(w);
        return std::string(bytes.data, bytes.size);
    }

    void updateApplication() {
        const auto describe = [&] {
            const Tab* active = model.tab_count() ? &model.tab(model.active()) : nullptr;
            const char* kind = !active ? "empty" : active->kind == TabKind::AI ? "ai" :
                active->kind == TabKind::Terminal ? "terminal" : active->kind == TabKind::Sprint ? "sprint" : "extension";
            if (!active || view.choiceTab != active->id || (view.secrets && view.secrets->showing())) {
                view.choiceKind.clear();
                view.choiceValue.clear();
            }
            if (view.accountPicker && (!active || active->id != view.accountPicker->tab ||
                    (view.secrets && view.secrets->showing())))
                view.accountPicker.reset();
            auto account = active && active->kind == TabKind::AI && view.signInAccount ? view.signInAccount() : SignInAccount{};
            if (view.dismissedSignIn && (!active || view.dismissedSignIn->first != active->id ||
                    view.dismissedSignIn->second != account.name)) view.dismissedSignIn.reset();
            view.windowApp.update(*view.application, kind, active ? active->type : std::string(), view.promptOpen,
                !view.status.empty(), model.active(), model.tab_count(), active ? active->group : std::string(),
                std::move(account),
                view.accountPicker.has_value(),
                active && active->kind == TabKind::AI && view.limitOffer ? view.limitOffer() : LimitOffer{}, policyUiState(active));
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
            case WindowCommand::ScrollTranscript: {
                PrLayoutScrollInfo info{};
                if (!view.transcript || !prLayoutNodeScrollInfo(tree, view.transcript, &info)) break;
                auto target = info.offset;
                if (key.type == "top") target.y = info.range.top;
                else if (key.type == "bottom") target.y = info.range.bottom;
                else {
                    const int64_t page = int64_t(info.portExtent.y.raw * look.ui.scroll_page_fraction);
                    const int64_t delta = key.type == "page_up" ? -page : page;
                    target.y.raw = static_cast<int32_t>(std::clamp<int64_t>(info.offset.y.raw + delta,
                        info.range.top.raw, info.range.bottom.raw));
                }
                prLayoutNodeSetScroll(tree, view.transcript, target);
                break;
            }
            case WindowCommand::ClearComposer:
                if (view.cancelGoal) view.cancelGoal();
                if (model.tab_count() && (!model.tab(model.active()).draft.empty() || !model.tab(model.active()).held.empty()))
                    view.chirp(view.windowApp.surface.discardChirp);
                model.clear_composer(model.active());
                view.prompted = ~std::size_t(0);
                break;
            case WindowCommand::ReplaceDraft:
                if (model.tab_count() && model.tab(model.active()).kind == TabKind::AI) {
                    model.set_draft(model.active(), key.type);
                    view.prompted = ~std::size_t(0);
                }
                break;
            case WindowCommand::SelectPrompt: {
                if (!model.tab_count() || model.tab(model.active()).kind != TabKind::AI ||
                        !view.prompt || prUiFocus(ui) != view.prompt || prUiTextEditIsComposing(ui, view.prompt)) break;
                auto selection = prUiTextEditSelection(ui, view.prompt);
                selection.caret = key.type == "start" ? 0 : static_cast<uint32_t>(model.tab(model.active()).draft.size());
                prUiTextEditSetSelection(ui, view.prompt, selection);
                break;
            }
            case WindowCommand::SelectTab: {
                const auto index = static_cast<std::size_t>(key.argument);
                select(index);
                break;
            }
            case WindowCommand::MoveTab: {
                const auto from = model.active();
                if (model.tab_count() == 0 || (key.argument < 0 && from == 0)) break;
                const auto to = key.argument < 0 ? from - 1 : from + 1;
                if (model.move_tab(from, to) && view.moved) view.moved();
                break;
            }
            case WindowCommand::ResolveLimit:
                if (!view.accountPicker && view.displayedLimit == view.windowApp.limitOffer && view.resolveLimit)
                    view.resolveLimit(view.displayedLimit, key.type);
                break;
            case WindowCommand::AccountAction:
                pickerInput = true;
                selectorAction(key.type);
                break;
            case WindowCommand::AccountPicker:
                if (view.pickerCommand) view.pickerCommand(key.type);
                if (key.type == "open" && model.tab_count() && model.tab(model.active()).draft == "/login") {
                    if (view.accountPicker) model.set_draft(model.active(), {});
                    else view.problem = look.words.connect_configured_account;
                    pickerInput = true;
                }
                break;
            case WindowCommand::AccountCommand:
                if (view.accountCommand) view.accountCommand(key.type);
                break;
            case WindowCommand::AccountMenu: {
                using Dialog = AccountMenuView::Dialog;
                pickerInput = true;
                const auto dialog = key.type == "login" ? Dialog::Login : key.type == "logout" ? Dialog::Logout : Dialog::Account;
                if (openAccountDialog(view, dialog) && model.tab_count()) model.set_draft(model.active(), {});
                break;
            }
            case WindowCommand::Zoom:
                view.askedTextSteps = policy::textZoom(*view.application, {}, view.askedTextSteps,
                    key.argument, int(std::ceil((look.ui.zoom_min - 1) / look.ui.zoom_step)),
                    int(std::floor((look.ui.zoom_max - 1) / look.ui.zoom_step)));
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
                if (tab.primaryChat()) break;
                const bool terminal = tab.kind == TabKind::Terminal;
                // Older window descriptions used separate primitives for terminals and extensions.
                const bool legacy = key.command == WindowCommand::CloseTerminal;
                if ((legacy && !terminal) || !view.windowApp.closable.value_or(legacy || tab.kind == TabKind::Extension))
                    break;
                const auto id = tab.id;
                if (tab.kind == TabKind::AI && view.dismissAI)
                    view.dismissAI(tab);
                const auto closing = model.active();
                if (terminal) {
                    if (closing > 0 && model.tab(closing - 1).group == tab.group)
                        model.select_tab(closing - 1);
                    else if (closing + 1 < model.tab_count() && model.tab(closing + 1).group == tab.group)
                        model.select_tab(closing + 1);
                }
                model.dismiss_tab(closing);
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
                        view.extensionProblem = look.words.service_required;
                    else
                        view.extensionProblem = view.requestService(type->second.service, tab.draft);
                }
                break;
            case WindowCommand::Stop:
                if (model.tab_count() && model.tab(model.active()).kind == TabKind::AI && view.stop)
                    view.stop();
                break;
        }
    }

    void page(PrLayoutNode window, bool composing) {
        view.conversation = kPrLayoutNodeNone;
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
        if (std::find(panels.begin(), panels.end(), Panel::Prompt) == panels.end())
            promptGone();
        if (view.windowApp.surface.items && !asking) window = element(window, Part::AIPanels);
        for (const auto panel : panels) {
            switch (panel) {
                case Panel::Board: sprint(window); break;
                case Panel::Terminal: terminal(window, model.tab(model.active())); break;
                case Panel::Spacer: prKitPlace(kit, prKitBox(kit, window, {}, {}), {.flex = PrKitFlex_Fill}); break;
                case Panel::Transcript: transcript(window); break;
                case Panel::Prompt: prompt(element(window, Part::Footer), composing); break;
                case Panel::LimitChoice: limitPanel(window); break;
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
                    caption(element(window, Part::ReplyRow), look.words.renderer_unavailable,
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
                        if (service.kind == TabKind::AI && service.name == type->second.service &&
                                !service.transcript.empty()) {
                            const auto& result = service.transcript.back();
                            if (result.kind == EntryKind::AI || (result.kind == EntryKind::Notice && look.transcriptStyle.showsNotice(result.text)))
                                caption(element(window, Part::ReplyRow), result.text, textStyle(PrKitTextRole_Body,
                                    static_cast<PrKitTone>(result.kind == EntryKind::Notice ? kNoticeTone : kChatTone)));
                            break;
                        }
                    }
                    break;
                }
                case Panel::Status:
                    caption(element(window, Part::Footer), view.status,
                        textStyle(PrKitTextRole_BodySmall, kStatusTone, PrKitTextAlign_Center));
                    // Where the window asks for its server (iOS), the way to change it, under the line that says how
                    // the link to it goes.
                    if (view.serverChoice != nullptr && !view.serverChoice->current().empty()) {
                        const auto button = prKitButton(kit, element(window, Part::Footer),
                            {.key = prHashString(sv("change-server"), 0), .text = sv(look.words.change_server)}, {});
                        if (asking == nullptr && !view.serverChoice->asking() && prKitActivated(kit, button))
                            view.serverChoice->change();
                    }
                    break;
            }
        }
        if (std::find(panels.begin(), panels.end(), Panel::Prompt) == panels.end() && model.tab_count() &&
                model.tab(model.active()).kind == TabKind::AI && view.goal &&
                !view.goal(model.tab(model.active()).id).empty())
            goalBubble(element(window, Part::Footer));
        if (!view.extensionProblem.empty() && model.tab_count() && model.tab(model.active()).kind == TabKind::Extension)
            caption(element(window, Part::ReplyRow), view.extensionProblem, textStyle(PrKitTextRole_BodySmall, kNoticeTone));
    }

    // Reuse the shaping scratch space while measuring the titles of a newly arrived tab group.
    std::unique_ptr<PrTextLayout, decltype(&prTextLayoutDestroy)> tabLayout{nullptr, prTextLayoutDestroy};
    std::unique_ptr<PrTextShaper, decltype(&prTextShaperDestroy)> fallbackTabShaper{nullptr, prTextShaperDestroy};

};

} // namespace

PrKitTheme windowTheme(PrFontSet* fonts, bool overlay, int steps, float opacity, const MessageCardStyle& card,
                       const TranscriptStyle& transcript, const TabStyle& tabs, const PromptStyle& prompt,
                       const UiMetrics& ui, const UiColors& colors, const UiFonts& families) {
    const float k = ui.scale(steps);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, fonts);
    if (!families.ui.empty()) theme.ui.name = sv(families.ui);
    if (!families.mono.empty()) theme.mono.name = sv(families.mono);
    // The palette is public, but which slot each widget draws with (the tab strip bg1, a hovered tab bg2, the strip's
    // rule line2) is read from the kit's source, not documented: Prax has no public per-widget colours (a request).
    PrKitPalette& p = theme.palette;
    p.bg0 = overlay ? faded(rgb(colors.background), opacity) : rgb(colors.background);   // the canvas
    p.bg1 = faded(rgb(tabs.fill), tabs.opacity);            // the tab strip
    p.bg2 = rgb(colors.control_hover);                                                    // a tab under the pointer
    p.line2 = rgb(colors.control_divider);                                                   // the strip's rule
    // These tones are used by the fork's transcript and status labels. Keep the palette for controls
    // and the inverse ink of the sign-in card independent of the conversation.
    p.axisX = rgb(transcript.ink);
    p.axisW = rgb(transcript.personInk);
    p.axisY = rgb(transcript.noticeInk);
    p.axisZ = rgb(transcript.statusInk);
    p.textInverse = rgb(card.ink);
    p.text1 = rgb(colors.control_ink);
    p.text3 = rgb(colors.control_muted_ink);
    p.text4 = rgb(colors.control_faint_ink);
    p.text2 = rgb(prompt.ink);
    p.textSelection = rgb(prompt.selection);
    // Explicit palette overrides are last, including alpha; the named surface settings above are the defaults.
    const auto overrideColor = [&](const char* key, PrColor& slot) {
        if (const auto found = colors.palette.find(key); found != colors.palette.end()) {
            slot = rgb(found->second >> 8);
            slot.a = float(found->second & 255) / 255;
        }
    };
    overrideColor("accent", p.accent);
    overrideColor("accentWeak", p.accentWeak);
    overrideColor("textSelection", p.textSelection);
    overrideColor("accentText", p.accentText);
    overrideColor("accentHover", p.accentHover);
    overrideColor("accentPress", p.accentPress);
    overrideColor("success", p.success);
    overrideColor("warning", p.warning);
    overrideColor("danger", p.danger);
    overrideColor("bg0", p.bg0);
    overrideColor("bg1", p.bg1);
    overrideColor("bg2", p.bg2);
    overrideColor("bg3", p.bg3);
    overrideColor("bg4", p.bg4);
    overrideColor("bgViewport", p.bgViewport);
    overrideColor("line1", p.line1);
    overrideColor("line2", p.line2);
    overrideColor("line3", p.line3);
    overrideColor("text1", p.text1);
    overrideColor("text2", p.text2);
    overrideColor("text3", p.text3);
    overrideColor("text4", p.text4);
    overrideColor("textInverse", p.textInverse);
    overrideColor("hudFill", p.hudFill);
    overrideColor("hudBorder", p.hudBorder);
    overrideColor("axisX", p.axisX);
    overrideColor("axisY", p.axisY);
    overrideColor("axisZ", p.axisZ);
    overrideColor("axisW", p.axisW);
    auto type = [&](PrKitTextRole role, float size, float lineHeight, bool mono = false) {
        PrKitType& t = theme.types[role];
        t.mono = mono;
        t.italic = false;
        t.size = size * k;
        t.weight = ui.normal_weight;
        t.lineHeight = lineHeight * k;
        t.letterSpacing = ui.letter_spacing;
    };
    type(PrKitTextRole_BodyItalic, ui.body_size, ui.line_pitch);
    theme.types[PrKitTextRole_BodyItalic].italic = true;
    type(PrKitTextRole_Title, ui.title_scale * ui.body_size, ui.title_scale * ui.line_pitch);
    type(PrKitTextRole_Heading, ui.heading_scale * ui.body_size, ui.heading_scale * ui.line_pitch);
    theme.types[PrKitTextRole_Title].weight = theme.types[PrKitTextRole_Heading].weight = ui.heading_weight;
    type(PrKitTextRole_Display, card.textSize, ui.display_line_height * card.textSize);
    type(PrKitTextRole_Body, ui.body_size, ui.line_pitch);                 // transcript, prompt, tabs
    type(kSelectedTabRole, ui.body_size, ui.line_pitch);
    theme.types[kSelectedTabRole].weight = tabs.selectedWeight;
    type(kTabRole, ui.body_size, ui.line_pitch);
    theme.types[kTabRole].weight = tabs.weight;
    type(kAccountRole, ui.body_size, ui.line_pitch);
    theme.types[kAccountRole].weight = card.unselectedWeight;
    type(PrKitTextRole_Strong, ui.body_size, ui.line_pitch);
    theme.types[PrKitTextRole_Strong].weight = static_cast<float>(static_cast<int>(card.emailWeight));
    type(PrKitTextRole_BodySmall, ui.label_size, ui.small_line_height * ui.label_size);    // the notice when there are no AIs
    type(PrKitTextRole_Meta, ui.meta_size, ui.meta_line_height * ui.meta_size);           // "queued"
    type(PrKitTextRole_Mono, ui.tool_size, ui.tool_pitch, true);           // a tool line
    return theme;
}

bool beginWindowFrame(View& view, PrKit* kit, PrFontSet* fonts) {
    Look& look = *view.look;
    const auto& surface = view.windowApp.surface;
    if (view.board) {
        view.board->setScrollStyle(surface.scroll);
        view.board->setBackgroundUpdates(surface.flags.dashboard_background_updates);
    }
    const bool uiChanged = look.ui != surface.ui || look.colors != surface.colors || look.words != surface.words || look.fonts != surface.fonts || look.flags != surface.flags;
    const bool scaleChanged = look.textSteps != view.askedTextSteps || look.ui != surface.ui;
    const bool opacityChanged = look.overlay && look.overlayOpacity != view.windowApp.overlayOpacity;
    // The prompt as the policy gives it, at the share of its width this screen takes (View::columnScale).
    PromptStyle askedPrompt = view.windowApp.surface.prompt;
    askedPrompt.width *= view.columnScale;
    // The transcript as the policy gives it, with the replies at the column's full width where the view asks for it
    // (View::fullWidthReplies).
    TranscriptStyle askedTranscript = view.windowApp.transcriptStyle;
    if (view.fullWidthReplies) askedTranscript.replyWidth = 100;
    const bool cardChanged = look.messageCard != view.windowApp.messageCard || look.tabs != view.windowApp.surface.tabs || look.prompt != askedPrompt;
    const bool transcriptChanged = look.transcriptStyle != askedTranscript;
    if (!scaleChanged && !opacityChanged && !cardChanged && !transcriptChanged && !uiChanged) return false;
    if (scaleChanged || cardChanged) view.tabWidths.clear();
    if (opacityChanged || cardChanged || transcriptChanged || uiChanged) {
        if (uiChanged || transcriptChanged) view.items.clear();
        look.ui = surface.ui;
        look.colors = surface.colors;
        look.words = surface.words;
        if (look.fonts != surface.fonts) view.clockFont.reset();
        look.fonts = surface.fonts;
        look.flags = surface.flags;
        if (look.transcriptStyle.showNotices != askedTranscript.showNotices ||
            look.transcriptStyle.hiddenNoticeFragments != askedTranscript.hiddenNoticeFragments ||
            look.transcriptStyle.showTools != askedTranscript.showTools ||
            look.transcriptStyle.codeSizeOffset != askedTranscript.codeSizeOffset ||
            look.transcriptStyle.listItemGapLines != askedTranscript.listItemGapLines ||
            look.transcriptStyle.listGap != askedTranscript.listGap ||
            look.transcriptStyle.replyWidth != askedTranscript.replyWidth ||
            look.prompt.width != askedPrompt.width)
            view.items.clear();
        look.transcriptStyle = askedTranscript;
        look.messageCard = view.windowApp.messageCard;
        look.tabs = view.windowApp.surface.tabs;
        look.prompt = askedPrompt;
        if (opacityChanged) look.overlayOpacity = view.windowApp.overlayOpacity;
        prUiForget(kit->ui, &look);
    }
    look.textSteps = view.askedTextSteps;
    PrKitTheme theme = windowTheme(fonts, look.overlay, look.textSteps, look.overlayOpacity, look.messageCard, look.transcriptStyle, look.tabs, look.prompt, look.ui, look.colors, look.fonts);
    prKitSetTheme(kit, &theme);
    if (!scaleChanged && !uiChanged) return true;
    if (view.terminals != nullptr) {
        view.terminals->setFontFamily(look.fonts.mono);
        view.terminals->setTextScale(look.ui.scale(look.textSteps) * look.ui.terminal_size / style::kTerminalSize);
    }
    if (view.board != nullptr)
        view.board->setTextScale(look.ui.scale(look.textSteps));
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
    return uiFormat(UiText{}.cut_prompt, {{"omitted", grouped(given - kept)}, {"given", grouped(given)}});
}

std::size_t promptRoom(const View& view, PrUi* ui) {
    const Model& model = *view.model;
    if (model.tab_count() == 0 || model.tab(model.active()).kind != TabKind::AI) {
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
    view.cut = uiFormat(view.look->words.cut_prompt, {{"omitted", grouped(given - kept)}, {"given", grouped(given)}});
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
    return view.look->ui != view.windowApp.surface.ui || view.look->colors != view.windowApp.surface.colors ||
           view.look->words != view.windowApp.surface.words || view.look->fonts != view.windowApp.surface.fonts ||
           view.look->prompt != view.windowApp.surface.prompt || view.look->tabs != view.windowApp.surface.tabs || view.look->transcriptStyle != view.windowApp.transcriptStyle ||
           view.look->messageCard != view.windowApp.messageCard || view.look->textSteps != view.askedTextSteps ||
           (view.look->overlay && view.look->overlayOpacity != view.windowApp.overlayOpacity) || view.breathing || view.energyAnimating || view.cutComing;
}

// Forward margin drags through the native editor so its UTF-8 selection, clipboard and editing agree.
bool routePromptPointer(View& view, PrUi* ui, const PrAppEvent& event) {
    const bool down = event.kind == PrAppEventKind_PointerDown && event.pointer.button == 0;
    if (down || event.kind == PrAppEventKind_PointerCancel) {
        view.promptPointerDown = false;
        view.marginAnchor.reset();
    }
    if (!down && !(view.promptPointerDown && (event.kind == PrAppEventKind_PointerMove ||
            event.kind == PrAppEventKind_PointerUp))) return false;
    if (event.kind == PrAppEventKind_PointerUp) view.promptPointerDown = false;
    if (!ui) return false;
    auto* tree = prUiTree(ui);
    const auto edit = view.prompt;
    if (!prLayoutNodeIsValid(tree, edit) || !prLayoutNodeIsValid(tree, view.promptBox)) return false;
    const auto coords = PrLayoutCoordinates_Viewport;
    const auto panel = prLayoutRectToPx(prLayoutNodeBounds(tree, view.promptBox, coords));
    const auto p = event.pointer.position;
    if (down) {
        const auto selected = prUiTextEditSelection(ui, edit);
        if (selected.anchor != selected.caret && !(event.modifiers & PrAppModifierFlag_Shift))
            prUiTextEditSetSelection(ui, edit, {selected.caret, selected.caret});
        if (p.x < panel.left || p.x >= panel.right || p.y < panel.top || p.y >= panel.bottom) return false;
        view.promptPointerDown = true;
    }
    const auto field = prLayoutRectToPx(prLayoutNodeBounds(tree, edit, coords));
    if (field.right - field.left < 2 || field.bottom - field.top < 2) return false;
    PrLayoutNode leaf = kPrLayoutNodeNone;
    textNodes(tree, edit, [&](PrLayoutNode node) { if (!leaf) leaf = node; });
    if (!leaf || prUiTextEditIsComposing(ui, edit)) return false;
    PrLayoutRect first{}, last{};
    if (!prLayoutNodeTextCaret(tree, leaf, 0, PrLayoutTextAffinity_Downstream, coords, &first) ||
        !prLayoutNodeTextCaret(tree, leaf, uint32_t(prLayoutNodeText(tree, leaf).size),
                               PrLayoutTextAffinity_Upstream, coords, &last)) return false;
    auto q = p;
    const auto start = prLayoutRectToPx(first), end = prLayoutRectToPx(last);
    if (p.y < start.top) q = {start.left, (start.top + start.bottom) / 2};
    else if (p.y >= end.bottom) q = {end.left, (end.top + end.bottom) / 2};
    q.x = std::clamp(q.x, field.left + 0.5f, field.right - 0.5f);
    q.y = std::clamp(q.y, field.top + 0.5f, field.bottom - 0.5f);
    if ((down && (q.x != p.x || q.y != p.y)) || view.marginAnchor) {
        uint32_t offset = 0;
        if (p.y >= end.bottom) offset = uint32_t(prLayoutNodeText(tree, leaf).size);
        else if (p.y >= start.top) {
            PrLayoutTextPosition hit{};
            if (prLayoutTextPositionFromPoint(tree, edit, {prLayoutUnitFromPx(q.x), prLayoutUnitFromPx(q.y)}, &hit))
                offset = hit.offset;
        }
        if (down) {
            view.marginAnchor = (event.modifiers & PrAppModifierFlag_Shift) ?
                prUiTextEditSelection(ui, edit).anchor : offset;
            prUiSetFocus(ui, edit);
        }
        prUiTextEditSetSelection(ui, edit, {*view.marginAnchor, offset});
        if (!view.promptPointerDown) view.marginAnchor.reset();
        prUiRequestFrame(ui);
        return true;
    }
    if (q.x == p.x && q.y == p.y) return false;
    auto redirected = event;
    redirected.pointer.position = q;
    prUiPushEvent(ui, kPrUiViewportMain, &redirected);
    return true;
}

bool openAccountDialog(View& view, AccountMenuView::Dialog dialog) {
    using Dialog = AccountMenuView::Dialog;
    auto& menu = view.accounts;
    if (!view.model || !view.model->tab_count() || view.model->tab(view.model->active()).kind != TabKind::AI) {
        view.problem = view.look->words.open_ai_account;
        return false;
    }
    const Tab& tab = view.model->tab(view.model->active());
    const std::string name(tab.displayName());
    menu.gesture.cancel();
    menu.dialogRows.clear();
    if (dialog == Dialog::Account) {
        const auto rows = view.accountMenuRows ? view.accountMenuRows() : std::nullopt;
        if (!rows) {
            view.problem = view.accountMenuRows ? uiFormat(view.look->words.account_switch_pending, {{"ai", name}}) :
                view.look->words.connect_choose_account;
            return false;
        }
        menu.dialogRows = *rows;
    } else if (dialog == Dialog::Login) {
        if (!view.accountLoginRows) {
            view.problem = view.look->words.connect_sign_in;
            return false;
        }
        menu.dialogRows = view.accountLoginRows();
    } else if (dialog == Dialog::Logout) {
        if (!view.logoutTarget || !view.logoutTarget(menu.logoutAccount, menu.logoutShown, menu.logoutNext)) {
            if (view.problem.empty()) view.problem = view.look->words.connect_sign_out;
            return false;
        }
    } else {
        return false;
    }
    view.problem.clear();
    menu.dialog = dialog;
    menu.loginHighlighted = 0;
    view.lastHighlightedAccount.clear();
    menu.dialogTab = tab.id;
    return true;
}

bool routeAccountMenu(View& view, PrUi* ui, const PrAppEvent& event) {
    auto& menu = view.accounts;
    auto& gesture = menu.gesture;
    gesture.setThreshold(view.windowApp.surface.accountMenu.tabMoveThreshold);
    const PrVec2 p = event.pointer.position;
    AccountMenuGesture::Step step;
    switch (event.kind) {
        case PrAppEventKind_PointerDown: {
            bool onAnchor = false;
            std::optional<std::vector<AccountMenuRow>> rows;
            const bool free = menu.dialog == AccountMenuView::Dialog::None &&
                (view.secrets == nullptr || view.secrets->showing() == nullptr) && !view.accountPicker &&
                view.choiceKind.empty();
            if (ui && free && event.pointer.button == 0 && view.model && view.model->tab_count() &&
                    view.model->tab(view.model->active()).kind == TabKind::AI && view.accountMenuRows) {
                auto* tree = prUiTree(ui);
                if (view.menuAnchor != kPrLayoutNodeNone && prLayoutNodeIsValid(tree, view.menuAnchor)) {
                    const auto r = prLayoutRectToPx(prLayoutNodeBounds(tree, view.menuAnchor, PrLayoutCoordinates_Viewport));
                    const MenuBox box{r.left, r.top, r.right, r.bottom};
                    if (box.contains(p.x, p.y) && (rows = view.accountMenuRows()) && !rows->empty()) {
                        onAnchor = true;
                        menu.anchor = box;
                        menu.textInset = -1;
                        if (view.menuAnchorText != kPrLayoutNodeNone && prLayoutNodeIsValid(tree, view.menuAnchorText)) {
                            const auto t = prLayoutRectToPx(prLayoutNodeBounds(tree, view.menuAnchorText,
                                                                               PrLayoutCoordinates_Viewport));
                            menu.textInset = std::max(0.0f, t.left - box.left);
                        }
                    }
                }
            }
            step = gesture.down(p.x, p.y, onAnchor);
            if (step.opened) {
                menu.tab = view.model->tab(view.model->active()).id;
                menu.rows = std::move(*rows);
                menu.layout = {};
                if (view.playChirp) view.playChirp(view.windowApp.surface.openChirp);
            }
            break;
        }
        case PrAppEventKind_PointerMove: {
            const auto before = gesture.hovered();
            step = gesture.move(p.x, p.y, menu.layout);
            if (step.startTabMove && ui) {
                // The press was the menu's; it becomes the tab's drag: the strip gets the press where it began, then
                // this movement, past the kit's own threshold, as if it had seen them all along.
                PrAppEvent press = event;
                press.kind = PrAppEventKind_PointerDown;
                press.pointer.position = PrVec2{gesture.originX(), gesture.originY()};
                prUiPushEvent(ui, kPrUiViewportMain, &press);
                prUiPushEvent(ui, kPrUiViewportMain, &event);
                prUiRequestFrame(ui);
                return true;
            }
            if (gesture.hovered() != before && gesture.hovered() && view.playChirp)
                view.playChirp(view.windowApp.surface.stepChirp);
            break;
        }
        case PrAppEventKind_PointerUp: {
            const auto rows = menu.rows;
            step = gesture.up(p.x, p.y, menu.layout);
            if (step.chosen && *step.chosen < rows.size()) {
                const auto& row = rows[*step.chosen];
                if (row.signIn) {
                    openAccountDialog(view, AccountMenuView::Dialog::Login);
                } else if (!row.current && view.switchAccount) {
                    if (view.playChirp) view.playChirp(view.windowApp.surface.confirmChirp);
                    view.switchAccount(row.account);
                }
            }
            break;
        }
        case PrAppEventKind_PointerCancel:
            step = gesture.cancel();
            break;
        default:
            return false;
    }
    if (step.closed) {
        menu.tab = ~std::size_t(0);
        menu.rows.clear();
    }
    if ((step.consumed || step.closed) && ui) prUiRequestFrame(ui);
    return step.consumed;
}

void buildWindow(View& view, PrKit* kit, PrLayoutNode parent) {
    Builder b{view, kit, kit->ui, kit->tree, *view.look, *view.model, {}};
    b.build(parent);
}

} // namespace nexus
