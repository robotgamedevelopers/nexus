#include "view.h"

#include <algorithm>
#include <string_view>

#include "pr/draw.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/ui_records.h"
#include "pr/ui_text_edit.h"

#include "input.h"
#include "style.h"

namespace nexus {

namespace {

using namespace style;

constexpr float kPi = 3.14159265358979f;

// Far below any transcript: a scroll offset that layout clamps to the bottom.
constexpr float kFar = 1.0e6f;

// ---------------------------------------------------------------------------------------------------------------------
// Records: one part per kind of node, and one recipe per group kind that switches on the part. Parts that hold text set
// the font and color groups; the others inherit them.

enum class Part : unsigned char {
    Window,
    Strip,
    Tab,
    TabActive,
    TabNumber,
    TabName,
    TabNameActive,
    Transcript,
    Column,
    Centered,
    Entry,
    Bubble,
    BubbleQueued,
    QueuedLabel,
    FriendText,
    NoticeText,
    Footer,
    Prompt,
    PromptScroll,
    PromptText,
    Placeholder,
    Plus,
    Stop,
    Mic,
    LabelRow,
    Label,
    Ring,
};

// The icons' natural sizes.
constexpr float kPlusSide = 0.7f * kU;
constexpr float kStopSide = 0.72f * kU;
constexpr float kMicWidth = 0.7f * kU;
constexpr float kMicHeight = 1.2f * kU;
constexpr float kRingSide = 0.76f * kLabelSize + 3;

// The prompt's padding: one line of text in a box of kPromptHeight, between 1 px borders.
constexpr float kPromptPadY = (kPromptHeight - 2 - kLinePitch) / 2;
constexpr float kPromptPadX = 0.8f * kU;

bool ownsText(Part p) {
    switch (p) {
        case Part::Window:
        case Part::TabNumber:
        case Part::TabName:
        case Part::TabNameActive:
        case Part::Centered:
        case Part::Bubble:
        case Part::BubbleQueued:
        case Part::QueuedLabel:
        case Part::FriendText:
        case Part::NoticeText:
        case Part::PromptText:
        case Part::Placeholder:
        case Part::Label:
            return true;
        default:
            return false;
    }
}

PrColor faded(PrColor c, float alpha) {
    c.a = alpha;
    return c;
}

PrLayoutLength px(float v) {
    return prLayoutLengthPx(v);
}

void border(PrLayoutBoxStyle& box, int side, float width) {
    box.border[side] = prLayoutUnitFromPx(width);
}

void borderColor(PrLayoutDecorationStyle& d, int side, PrColor c) {
    d.borderColor[side] = prColor16FromColor(c);
    d.borderStyle[side] = PrDrawBorderStyle_Solid;
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

struct FontRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Font;
    Part part;

    void make(const Look& look, PrLayoutFontStyle& font) const {
        font.font = look.body;
        font.lineHeight = prLayoutLineHeightPx(kLinePitch);
        font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
        font.textWrapMode = PrLayoutTextWrapMode_Wrap;
        font.overflowWrap = PrTextOverflowWrap_BreakWord;
        switch (part) {
            case Part::TabNumber:
            case Part::TabName:
            case Part::TabNameActive:
                font.font = look.tab;
                font.lineHeight = prLayoutLineHeightPx(1.2f * kTabSize);
                font.textWrapMode = PrLayoutTextWrapMode_Nowrap;
                break;
            case Part::Label:
                font.font = look.label;
                font.lineHeight = prLayoutLineHeightPx(1.2f * kLabelSize);
                font.textWrapMode = PrLayoutTextWrapMode_Nowrap;
                break;
            case Part::QueuedLabel:
                font.font = look.meta;
                font.lineHeight = prLayoutLineHeightPx(1.3f * kMetaSize);
                break;
            case Part::Centered:
                font.textAlign = font.textAlignLast = PrLayoutTextAlign_Center;
                break;
            case Part::Placeholder:
                font.textWrapMode = PrLayoutTextWrapMode_Nowrap;
                break;
            default:
                break;
        }
    }
};

struct ColorRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Color;
    Part part;

    void make(const Look&, PrLayoutColorStyle& color) const {
        PrColor c = kInk;
        switch (part) {
            case Part::TabNumber: c = kPlaceholder; break;
            case Part::TabName: c = kNotice; break;
            case Part::BubbleQueued: c = faded(kInk, kQueuedInk); break;
            case Part::QueuedLabel: c = kMeta; break;
            case Part::NoticeText: c = kNotice; break;
            case Part::Centered: c = kPlaceholder; break;
            case Part::Placeholder: c = kPlaceholder; break;
            default: break;
        }
        color.color = prColor16FromColor(c);
        if (part == Part::Placeholder)
            color.pointerEvents = PrLayoutPointerEvents_None;
    }
};

struct BoxRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    Part part;

    void make(const Look&, PrLayoutBoxStyle& box) const {
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        switch (part) {
            case Part::Window:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(100);
                box.height = prLayoutLengthPercent(100);
                break;
            case Part::Strip:
                box.display = PrLayoutDisplay_Flex;
                box.height = px(kStripHeight + 1);
                border(box, PrRectSide_Bottom, 1);
                padding(box, 0, 0, 0, 0.8f * kU);
                break;
            case Part::Tab:
            case Part::TabActive:
                box.display = PrLayoutDisplay_Flex;
                box.height = px(1.75f * kTabSize);
                padding(box, 0, 0.6f * kTabSize, 0, 0.6f * kTabSize);
                break;
            case Part::Transcript:
                box.display = PrLayoutDisplay_Block;
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
                padding(box, kColumnInset, 0, 0, 0);
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
                padding(box, kBubblePadY, kBubblePadX, kBubblePadY, kBubblePadX);
                break;
            case Part::QueuedLabel:
                box.display = PrLayoutDisplay_Block;
                box.margin[PrRectSide_Top] = px(0.3f * kMetaSize);
                box.margin[PrRectSide_Right] = px(6);
                break;
            case Part::FriendText:
            case Part::NoticeText:
                box.display = PrLayoutDisplay_Block;
                break;
            case Part::Footer:
                box.display = PrLayoutDisplay_Flex;
                padding(box, kU, 0, 0, 0);
                break;
            case Part::Prompt:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kPromptPercent);
                box.minHeight = px(kPromptHeight);
                for (int side = 0; side < PrRectSide_Count; ++side)
                    border(box, side, 1);
                padding(box, kPromptPadY, kPromptPadX, kPromptPadY, kPromptPadX);
                break;
            case Part::PromptScroll:
                box.display = PrLayoutDisplay_Block;
                box.minWidth = px(0);
                box.maxHeight = px(kPromptMaxLines * kLinePitch);
                box.overflowX = PrLayoutOverflow_Hidden;
                box.overflowY = PrLayoutOverflow_Scroll;
                break;
            case Part::PromptText:
                box.display = PrLayoutDisplay_Block;
                box.position = PrLayoutPosition_Relative;
                break;
            case Part::Placeholder:
                box.display = PrLayoutDisplay_Block;
                box.position = PrLayoutPosition_Absolute;
                box.inset[PrRectSide_Top] = px(0);
                box.inset[PrRectSide_Left] = px(0);
                break;
            case Part::Plus:
                box.margin[PrRectSide_Right] = px(0.8f * kU);
                box.margin[PrRectSide_Bottom] = px((kLinePitch - kPlusSide) / 2);
                break;
            case Part::Stop:
                box.margin[PrRectSide_Left] = px(0.8f * kU);
                box.margin[PrRectSide_Bottom] = px((kLinePitch - kStopSide) / 2);
                break;
            case Part::Mic:
                box.margin[PrRectSide_Left] = px(1.9f * kU - kStopSide / 2 - kMicWidth / 2);
                box.margin[PrRectSide_Bottom] = px((kLinePitch - kMicHeight) / 2);
                break;
            case Part::LabelRow:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kPromptPercent);
                box.height = px(kPromptBottom);
                padding(box, 0, 0.7f * kU, kLabelBaseline - 0.32f * kLabelSize, 0);
                break;
            case Part::Ring:
                box.margin[PrRectSide_Left] = px(kLabelSize);
                break;
            default:
                break;
        }
    }
};

struct DecorationRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    Part part;

    void make(const Look&, PrLayoutDecorationStyle& d) const {
        switch (part) {
            case Part::Window:
                d.background = prColor16FromColor(kBackground);
                break;
            case Part::Strip:
                d.background = prColor16FromColor(kStrip);
                borderColor(d, PrRectSide_Bottom, kStripLine);
                break;
            case Part::TabActive:
                d.background = prColor16FromColor(kBubble);
                radius(d, 1.75f * kTabSize * 0.3f);
                break;
            case Part::Bubble:
                d.background = prColor16FromColor(kBubble);
                radius(d, kBubbleRadius);
                break;
            case Part::BubbleQueued:
                d.background = prColor16FromColor(faded(kBubble, kQueuedFill));
                radius(d, kBubbleRadius);
                break;
            case Part::Prompt:
                d.background = prColor16FromColor(kPromptFill);
                for (int side = 0; side < PrRectSide_Count; ++side)
                    borderColor(d, side, kPromptBorder);
                radius(d, kPromptRadius);
                break;
            default:
                break;
        }
    }
};

struct FlexRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    Part part;

    void make(const Look&, PrLayoutFlexStyle& flex) const {
        switch (part) {
            case Part::Window:
                flex.direction = PrLayoutFlexDirection_Column;
                break;
            case Part::Strip:
                flex.direction = PrLayoutFlexDirection_Row;
                flex.alignItems = PrLayoutAlignment_Center;
                flex.columnGap = px(0.3f * kTabSize);
                flex.shrink = 0;
                break;
            case Part::Tab:
            case Part::TabActive:
                flex.direction = PrLayoutFlexDirection_Row;
                flex.alignItems = PrLayoutAlignment_Center;
                flex.columnGap = px(0.45f * kTabSize);
                break;
            case Part::Transcript:
                flex.grow = 1;
                flex.basis = px(0);
                break;
            case Part::Column:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.justifyContent = PrLayoutAlignment_FlexEnd;
                flex.rowGap = px(kGap);
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
                break;
            case Part::PromptScroll:
                flex.grow = 1;
                flex.basis = px(0);
                flex.alignSelf = PrLayoutAlignment_Center;
                break;
            case Part::Plus:
            case Part::Stop:
            case Part::Mic:
                flex.shrink = 0;
                break;
            case Part::LabelRow:
                flex.direction = PrLayoutFlexDirection_Row;
                flex.justifyContent = PrLayoutAlignment_FlexEnd;
                flex.alignItems = PrLayoutAlignment_FlexEnd;
                break;
            default:
                break;
        }
    }
};

PrLayoutStyleIndex style(PrUi* ui, const Look& look, Part p) {
    if (ownsText(p))
        return pr::uiStyle(ui, look, FontRecipe{p}, ColorRecipe{p}, BoxRecipe{p}, DecorationRecipe{p}, FlexRecipe{p});
    return pr::uiStyle(ui, look, BoxRecipe{p}, DecorationRecipe{p}, FlexRecipe{p});
}

// ---------------------------------------------------------------------------------------------------------------------
// Icons, drawn once into lists in their natural coordinates.

void line(PrDrawList* list, PrVec2 a, PrVec2 b, PrColor color, float width) {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.shape.startCap = PrPathCap_Round;
    stroke.shape.endCap = PrPathCap_Round;
    prDrawListLine(list, a, b, PrDrawPaint{.color = color, .stroke = &stroke});
}

PrDrawList* drawPlus() {
    PrDrawList* list = prDrawListCreate();
    float c = kPlusSide / 2;
    float arm = 0.32f * kU;
    line(list, PrVec2{c - arm, c}, PrVec2{c + arm, c}, kInk, 2);
    line(list, PrVec2{c, c - arm}, PrVec2{c, c + arm}, kInk, 2);
    return list;
}

PrDrawList* drawStop() {
    PrDrawList* list = prDrawListCreate();
    PrCornerRect shape{};
    shape.rect = PrRect{0, 0, kStopSide, kStopSide};
    for (PrCorner& corner : shape.corners)
        corner = PrCorner{PrVec2{kStopSide * 0.15f, kStopSide * 0.15f}, 1};
    prDrawListCornerRect(list, shape, PrDrawPaint{.color = kStop});
    return list;
}

PrDrawList* drawMic() {
    PrDrawList* list = prDrawListCreate();
    float x = kMicWidth / 2;
    float m = 0.2f * kU;
    float mid = kMicHeight * 0.45f;
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 2;
    prDrawListCapsule(list, PrVec2{x, mid - m * 2}, PrVec2{x, mid + m * 0.2f}, m * 1.1f,
        PrDrawPaint{.color = kInk, .stroke = &stroke});
    prDrawListSector(list, PrVec2{x, mid + m * 0.2f}, m * 1.8f, m * 1.8f + 2, 0, kPi, PrDrawPaint{.color = kInk});
    line(list, PrVec2{x, mid + m * 2.0f}, PrVec2{x, mid + m * 2.8f}, kInk, 2);
    return list;
}

PrDrawList* drawRing() {
    PrDrawList* list = prDrawListCreate();
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 2.2f;
    float r = 0.38f * kLabelSize;
    prDrawListEllipse(list, PrVec2{kRingSide / 2, kRingSide / 2}, PrVec2{r, r}, 0,
        PrDrawPaint{.color = kRing, .stroke = &stroke});
    return list;
}

PrTextFont* font(PrFontSet* set, float size) {
    PrFontFamily family = prFontFamilyGeneric(PrFontGeneric_SystemUi);
    PrFontStyle s = prFontStyleDefault();
    s.size = size;
    PrTextFontDesc desc{.set = set, .families = &family, .familyCount = 1, .style = s};
    return prTextFontCreate(&desc);
}

// ---------------------------------------------------------------------------------------------------------------------
// The tree.

PrStringView sv(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

constexpr std::string_view kPlaceholderText = "\xE2\x8F\x8E to send, \xE2\x8C\x98\xE2\x8F\x8E to queue for later";

struct Builder {
    View& view;
    PrUi* ui;
    PrLayoutTree* tree;
    const Look& look;
    Model& model;

    PrLayoutNode element(PrLayoutNode parent, Part p, PrUiFlags flags = 0, PrUiKey key = 0) {
        return prUiElement(ui, parent, key, style(ui, look, p), flags);
    }

    PrLayoutNode text(PrLayoutNode parent, Part p, PrStringView value) {
        PrLayoutNode node = element(parent, p);
        prLayoutTreeFrameText(tree, node, 0, value);
        return node;
    }

    void icon(PrLayoutNode parent, Part p, PrDrawList* list, float width, float height) {
        PrLayoutNode node = prLayoutTreeFrameReplaced(tree, parent, 0, style(ui, look, p));
        prLayoutTreeFrameSetNaturalSize(tree, node,
            PrLayoutNaturalSize{prLayoutUnitFromPx(width), prLayoutUnitFromPx(height), 0});
        PrLayoutContent content{};
        content.kind = PrLayoutContentKind_DrawList;
        content.list = list;
        prLayoutTreeFrameSetContent(tree, node, &content);
    }

    void strip(PrLayoutNode window) {
        PrLayoutNode row = element(window, Part::Strip);
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            PrLayoutNode tab = element(row, Part::Tab, PrUiFlag_Pressable, prUiKeyIndex(ui, row, i));
            if (prUiClicked(ui, tab, 0))
                model.select_tab(i);
            bool active = i == model.active();
            prLayoutTreeFrameSetStyle(tree, tab, style(ui, look, active ? Part::TabActive : Part::Tab));
            std::string number = std::to_string(i + 1);
            text(tab, Part::TabNumber, sv(number));
            text(tab, active ? Part::TabNameActive : Part::TabName, sv(model.tab(i).name));
        }
    }

    // Keeps each tab's scroll while another shows, and keeps a following transcript at its newest entry.
    void scroll(PrLayoutNode scroller, std::size_t active) {
        PrLayoutScrollInfo info{};
        bool laid = prLayoutNodeScrollInfo(tree, scroller, &info) && info.scrollContainer;
        float stored = prLayoutUnitToPx(prLayoutNodeScroll(tree, scroller).y);
        float bottom = laid ? prLayoutUnitToPx(info.range.bottom) : 0;
        TabView& tv = view.tabs[active];
        if (view.shown != active) {
            if (view.shown < view.tabs.size() && laid)
                view.tabs[view.shown].scroll = std::min(stored, bottom);
            view.shown = active;
            prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0),
                prLayoutUnitFromPx(tv.follow ? kFar : tv.scroll)});
        } else if (laid) {
            tv.follow = stored >= bottom - 0.5f;
        }
        std::size_t count = model.tab(active).transcript.size();
        if (count != tv.seen) {
            tv.seen = count;
            tv.follow = true;
        }
        if (tv.follow && stored < kFar)
            prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(kFar)});
    }

    void transcript(PrLayoutNode window) {
        PrLayoutNode scroller = element(window, Part::Transcript, PrUiFlag_Scrollable);
        PrLayoutNode column = element(scroller, Part::Column);
        if (model.tab_count() == 0) {
            text(column, Part::Centered, sv(view.notice));
            return;
        }
        std::size_t active = model.active();
        scroll(scroller, active);
        const Tab& tab = model.tab(active);
        if (tab.transcript.empty()) {
            std::string hint = "Say something to " + tab.name;
            text(column, Part::Centered, sv(hint));
        }
        for (const Entry& e : tab.transcript) {
            switch (e.kind) {
                case EntryKind::Person:
                case EntryKind::Queued: {
                    bool queued = e.kind == EntryKind::Queued;
                    PrLayoutNode entry = element(column, Part::Entry);
                    text(entry, queued ? Part::BubbleQueued : Part::Bubble, sv(e.text));
                    if (queued)
                        text(entry, Part::QueuedLabel, PrStringView{"queued", 6});
                    break;
                }
                case EntryKind::Friend:
                    text(column, Part::FriendText, sv(e.text));
                    break;
                case EntryKind::Notice:
                    text(column, Part::NoticeText, sv(e.text));
                    break;
            }
        }
    }

    // Shift-Enter: a line feed in place of the selection, with the caret after it.
    void newline(PrLayoutNode prompt, std::size_t active) {
        std::string draft = model.tab(active).draft;
        PrUiTextSelection sel = prUiTextEditSelection(ui, prompt);
        std::size_t lo = std::min<std::size_t>(std::min(sel.anchor, sel.caret), draft.size());
        std::size_t hi = std::min<std::size_t>(std::max(sel.anchor, sel.caret), draft.size());
        draft.replace(lo, hi - lo, "\n");
        model.set_draft(active, std::move(draft));
        uint32_t at = uint32_t(lo + 1);
        prUiTextEditSetSelection(ui, prompt, PrUiTextSelection{at, at});
    }

    void prompt(PrLayoutNode footer) {
        std::size_t active = model.active();
        PrLayoutNode box = element(footer, Part::Prompt, PrUiFlag_Pressable | PrUiFlag_Focusable,
            prUiKey(ui, kPrLayoutNodeNone, PrStringView{"prompt", 6}));
        prUiSetCursor(ui, box, PrAppCursor_Text);
        if (prUiFocus(ui) != box)
            prUiSetFocus(ui, box);
        icon(box, Part::Plus, look.plus, kPlusSide, kPlusSide);
        PrLayoutNode scroller = element(box, Part::PromptScroll, PrUiFlag_Scrollable);
        PrLayoutNode holder = element(scroller, Part::PromptText);

        // A tab's draft comes back with the caret at its end.
        const std::string& draft = model.tab(active).draft;
        if (view.prompted != active) {
            view.prompted = active;
            uint32_t end = uint32_t(draft.size());
            prUiTextEditSetSelection(ui, box, PrUiTextSelection{end, end});
        }
        PrUiTextEditDesc edit{};
        edit.control = box;
        edit.parent = holder;
        edit.value = sv(draft);
        edit.flags = PrUiTextEditFlag_Multiline;
        edit.selection = prColor16FromColor(kSelection);
        edit.caret = prColor16FromColor(kInk);
        prUiTextEdit(ui, &edit);
        PrStringView changed{};
        if (prUiTextEditChanged(ui, box, &changed))
            model.set_draft(active, std::string(changed.data, changed.size));

        std::size_t count = 0;
        const PromptKey* keys = promptKeys(&count);
        for (std::size_t i = 0; i < count; ++i) {
            if (!prUiKeyCaptured(ui, box, keys[i].key, keys[i].modifiers))
                continue;
            switch (keys[i].action) {
                case Action::Send: model.send(active); break;
                case Action::Queue: model.queue(active); break;
                case Action::Newline: newline(box, active); break;
                default: break;
            }
        }
        if (model.tab(active).draft.empty() && !prUiTextEditIsComposing(ui, box))
            text(holder, Part::Placeholder, sv(kPlaceholderText));
        icon(box, Part::Stop, look.stop, kStopSide, kStopSide);
        icon(box, Part::Mic, look.mic, kMicWidth, kMicHeight);

        PrLayoutNode labels = element(footer, Part::LabelRow);
        text(labels, Part::Label, sv(model.tab(active).model));
        icon(labels, Part::Ring, look.ring, kRingSide, kRingSide);
    }

    void build(PrLayoutNode parent) {
        view.tabs.resize(model.tab_count());
        PrLayoutNode window = element(parent, Part::Window);
        strip(window);
        transcript(window);
        PrLayoutNode footer = element(window, Part::Footer);
        if (model.tab_count() != 0)
            prompt(footer);
    }
};

} // namespace

void openLook(Look& look, PrFontSet* fonts) {
    look.body = font(fonts, kBodySize);
    look.tab = font(fonts, kTabSize);
    look.label = font(fonts, kLabelSize);
    look.meta = font(fonts, kMetaSize);
    look.plus = drawPlus();
    look.stop = drawStop();
    look.mic = drawMic();
    look.ring = drawRing();
}

void closeLook(Look& look) {
    prTextFontRelease(look.body);
    prTextFontRelease(look.tab);
    prTextFontRelease(look.label);
    prTextFontRelease(look.meta);
    prDrawListRelease(look.plus);
    prDrawListRelease(look.stop);
    prDrawListRelease(look.mic);
    prDrawListRelease(look.ring);
    look = Look{};
}

void buildWindow(View& view, PrUi* ui, PrLayoutNode parent) {
    Builder b{view, ui, prUiTree(ui), *view.look, *view.model};
    b.build(parent);
}

} // namespace nexus
