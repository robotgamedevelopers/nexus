#include "view.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "pr/draw.h"
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
// bubbles, and the rows that place the kit's parts. One part per kind of node, and one recipe per group kind that
// switches on the part. Their text is the kit's (labels in the theme's roles), so no record here sets a font.

enum class Part : unsigned char {
    Window,
    Transcript,
    Column,
    Centered,
    Entry,
    Bubble,
    BubbleQueued,
    Footer,
    PromptRow,
    LabelRow,
    Ring,
};

// The kit's icons' sides, and the ring's.
constexpr float kPlusSide = 0.7f * kU;
constexpr float kStopSide = 0.72f * kU;
constexpr float kRingSide = 0.76f * kLabelSize + 3;

// The height of the kit text area with one line, which the icons beside it centre on: the line, the area's padding
// of 4 px above and below (kit_input.cpp kAreaPadding), and its 1 px borders.
constexpr float kOneLineArea = kLinePitch + 2 * 4 + 2;

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
            case Part::Footer:
                box.display = PrLayoutDisplay_Flex;
                padding(box, kU, 0, 0, 0);
                break;
            case Part::PromptRow:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kPromptPercent);
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
        }
    }
};

struct DecorationRecipe {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    Part part;

    void make(const Look& look, PrLayoutDecorationStyle& d) const {
        switch (part) {
            case Part::Window:
                d.background = prColor16FromColor(look.overlay ? faded(kBackground, kOverlayOpacity) : kBackground);
                break;
            case Part::Bubble:
                d.background = prColor16FromColor(kBubble);
                radius(d, kBubbleRadius);
                break;
            case Part::BubbleQueued:
                d.background = prColor16FromColor(faded(kBubble, kQueuedFill));
                radius(d, kBubbleRadius);
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
            case Part::Transcript:
                // Reversed, the scroller's origin is its bottom (css-overflow-3 scroll origin): it starts at the newest
                // entry and stays there as entries arrive, and scroll anchoring holds a view scrolled up in place.
                flex.direction = PrLayoutFlexDirection_ColumnReverse;
                flex.grow = 1;
                flex.basis = px(0);
                break;
            case Part::Column:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.justifyContent = PrLayoutAlignment_FlexEnd;
                flex.rowGap = px(kGap);
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
            case Part::PromptRow:
                flex.direction = PrLayoutFlexDirection_Row;
                flex.alignItems = PrLayoutAlignment_FlexEnd;
                flex.columnGap = px(0.8f * kU);
                break;
            case Part::LabelRow:
                flex.direction = PrLayoutFlexDirection_Row;
                flex.justifyContent = PrLayoutAlignment_FlexEnd;
                flex.alignItems = PrLayoutAlignment_FlexEnd;
                break;
            case Part::Ring:
                flex.shrink = 0;
                break;
            default:
                break;
        }
    }
};

PrLayoutStyleIndex style(PrUi* ui, const Look& look, Part p) {
    return pr::uiStyle(ui, look, BoxRecipe{p}, DecorationRecipe{p}, FlexRecipe{p});
}

// The context ring beside the model label: no kit part or icon shows a ring.
PrDrawList* drawRing() {
    PrDrawList* list = prDrawListCreate();
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 2.2f;
    float r = 0.38f * kLabelSize;
    prDrawListEllipse(list, PrVec2{kRingSide / 2, kRingSide / 2}, PrVec2{r, r}, 0,
        PrDrawPaint{.color = kRing, .stroke = &stroke});
    return list;
}

// ---------------------------------------------------------------------------------------------------------------------
// The tree.

PrStringView sv(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

constexpr std::string_view kPlaceholderText = "\xE2\x8F\x8E to send, \xE2\x8C\x98\xE2\x8F\x8E to queue for later";

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

    PrLayoutNode element(PrLayoutNode parent, Part p, PrUiFlags flags = 0, PrUiKey key = 0) {
        return prUiElement(ui, parent, key, style(ui, look, p), flags);
    }

    PrLayoutNode label(PrLayoutNode parent, std::string_view text, PrKitTextStyle s) {
        return prKitLabel(kit, parent, PrKitLabel{.key = 0, .text = sv(text)}, s);
    }

    void icon(PrLayoutNode parent, PrKitIconKind kind, float side) {
        PrLayoutNode node = prKitIcon(kit, parent, PrKitIcon{.key = 0, .icon = kind},
            PrKitIconStyle{.size = side, .tone = PrKitTone_Primary, ._pad = {}});
        // Centred on the prompt's first line, where the row aligns its items at the bottom.
        PrKitPlace place{};
        place.margin.bottom = (kOneLineArea - side) / 2;
        place.flex = PrKitFlex_Fixed;
        prKitPlace(kit, node, &place);
    }

    void strip(PrLayoutNode window) {
        PrLayoutNode row = prKitTabStrip(kit, window, PrKitTabStrip{.key = 0, .disabled = false, .reorderable = false},
            PrKitChoiceStyle{.size = PrKitControlSize_Lg, .direction = PrKitDirection_Row});
        PrKitPlace rowPlace{};
        rowPlace.height = prKitPx(kStripHeight + 1);
        rowPlace.flex = PrKitFlex_Fixed;
        prKitPlace(kit, row, &rowPlace);
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            PrLayoutNode tab = prKitTab(kit, row, PrKitTab{.key = prUiKeyIndex(ui, row, i),
                .selected = i == model.active(), .disabled = false});
            PrKitPlace tabPlace{};
            tabPlace.height = prKitPx(1.75f * kTabSize);
            tabPlace.align = PrKitAlign_Center;
            if (i == 0)
                tabPlace.margin.left = 0.8f * kU;
            prKitPlace(kit, tab, &tabPlace);
            if (prKitActivated(kit, tab))
                model.select_tab(i);
            std::string number = std::to_string(i + 1);
            label(tab, number, textStyle(PrKitTextRole_Body, PrKitTone_Faint));
            prKitTabLabel(kit, tab, PrKitLabel{.key = 0, .text = sv(model.tab(i).name)});
        }
    }

    // Each tab keeps its scroll while another shows. Offsets count from the scroller's origin, its bottom, so a tab
    // never scrolled stays at its newest entry.
    void keepScroll(PrLayoutNode scroller, std::size_t active) {
        view.scrolls.resize(model.tab_count(), 0);
        if (view.shown == active)
            return;
        if (view.shown < view.scrolls.size() && view.transcript != kPrLayoutNodeNone)
            view.scrolls[view.shown] = prLayoutUnitToPx(prLayoutNodeScroll(tree, view.transcript).y);
        view.shown = active;
        prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(view.scrolls[active])});
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
        std::size_t active = model.active();
        keepScroll(scroller, active);
        view.transcript = scroller;
        const Tab& tab = model.tab(active);
        if (tab.transcript.empty()) {
            PrLayoutNode centered = element(column, Part::Centered);
            std::string hint = "Say something to " + tab.name;
            label(centered, hint, textStyle(PrKitTextRole_Body, PrKitTone_Faint, PrKitTextAlign_Center));
        }
        for (const Entry& e : tab.transcript) {
            switch (e.kind) {
                case EntryKind::Person:
                case EntryKind::Queued: {
                    bool queued = e.kind == EntryKind::Queued;
                    PrLayoutNode entry = element(column, Part::Entry);
                    PrLayoutNode bubble = element(entry, queued ? Part::BubbleQueued : Part::Bubble);
                    label(bubble, e.text, textStyle(PrKitTextRole_Body, queued ? PrKitTone_Muted : PrKitTone_Primary));
                    if (queued) {
                        PrLayoutNode tag = label(entry, "queued", textStyle(PrKitTextRole_Meta, PrKitTone_Faint));
                        PrKitPlace place{};
                        place.margin.top = 0.3f * kMetaSize;
                        place.margin.right = 6;
                        prKitPlace(kit, tag, &place);
                    }
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
    }

    // The kit's text area has a fixed number of rows; the prompt grows with its text up to kPromptMaxLines, so the rows
    // come from how tall the text was at the last layout. A workaround for the area's missing auto height.
    uint32_t promptRows() {
        if (view.prompt == kPrLayoutNodeNone)
            return 1;
        PrLayoutNode text = prLayoutNodeFirstChild(tree, view.prompt);
        if (text == kPrLayoutNodeNone)
            return view.promptRows;
        PrLayoutRect box = prLayoutNodeBounds(tree, text, PrLayoutCoordinates_Layout);
        float lines = std::round(prLayoutUnitToPx(box.bottom - box.top) / kLinePitch);
        return uint32_t(std::clamp(lines, 1.0f, kPromptMaxLines));
    }

    void prompt(PrLayoutNode footer) {
        std::size_t active = model.active();
        // Enter sends and Command-Enter queues. Captured on the row that holds the text area, so the area never sees
        // them; every other Enter (Shift-Enter among them) goes to the area, which breaks the line (pr/ui.h).
        PrLayoutNode row = element(footer, Part::PromptRow);
        bool send = prUiKeyCaptured(ui, row, PrAppKey_Enter, 0);
        bool queue = prUiKeyCaptured(ui, row, PrAppKey_Enter, PrAppModifierFlag_Super);
        // While an input method composes, its Enter commits the composition (CompositionEnd, then Text, after the
        // KeyDown): read before the area takes this round's events.
        bool composing = view.prompt != kPrLayoutNodeNone && prUiTextEditIsComposing(ui, view.prompt);
        view.promptRows = promptRows();

        icon(row, PrKitIconKind_Plus, kPlusSide);
        PrKitTextArea area{};
        area.key = prUiKey(ui, kPrLayoutNodeNone, PrStringView{"prompt", 6});
        area.text = sv(model.tab(active).draft);
        area.placeholder = sv(kPlaceholderText);
        area.rows = view.promptRows;
        PrLayoutNode edit = prKitTextArea(kit, row, area, PrKitFieldStyle{});
        PrKitPlace fill{};
        fill.flex = PrKitFlex_Fill;
        prKitPlace(kit, edit, &fill);
        view.prompt = edit;
        if (prUiFocus(ui) != edit)
            prUiSetFocus(ui, edit);
        PrStringView changed{};
        if (prKitTextChanged(kit, edit, &changed))
            model.set_draft(active, std::string(changed.data, changed.size));
        if (!composing && send)
            model.send(active);
        if (!composing && queue)
            model.queue(active);
        // A tab's draft comes back with the caret at its end.
        if (view.prompted != active) {
            view.prompted = active;
            uint32_t end = uint32_t(model.tab(active).draft.size());
            prUiTextEditSetSelection(ui, edit, PrUiTextSelection{end, end});
        }
        icon(row, PrKitIconKind_Stop, kStopSide);

        PrLayoutNode labels = element(footer, Part::LabelRow);
        label(labels, model.tab(active).model, textStyle(PrKitTextRole_BodySmall, PrKitTone_Primary));
        PrLayoutNode ring = prLayoutTreeFrameReplaced(tree, labels, 0, style(ui, look, Part::Ring));
        prLayoutTreeFrameSetNaturalSize(tree, ring,
            PrLayoutNaturalSize{prLayoutUnitFromPx(kRingSide), prLayoutUnitFromPx(kRingSide), 0});
        PrLayoutContent content{};
        content.kind = PrLayoutContentKind_DrawList;
        content.list = look.ring;
        prLayoutTreeFrameSetContent(tree, ring, &content);
    }

    void build(PrLayoutNode parent) {
        PrLayoutNode window = element(parent, Part::Window);
        strip(window);
        transcript(window);
        PrLayoutNode footer = element(window, Part::Footer);
        if (model.tab_count() != 0)
            prompt(footer);
    }
};

} // namespace

void openLook(Look& look) {
    look.ring = drawRing();
}

void closeLook(Look& look) {
    prDrawListRelease(look.ring);
    look = Look{};
}

PrKitTheme windowTheme(PrFontSet* fonts, bool overlay) {
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, fonts);
    PrKitPalette& p = theme.palette;
    p.bg0 = overlay ? faded(kBackground, kOverlayOpacity) : kBackground;   // the canvas
    p.bg1 = overlay ? faded(kStrip, kOverlayStripFill) : kStrip;            // the tab strip
    p.bg2 = kPromptFill;                                                    // the prompt's fill, a tab under the pointer
    p.line2 = kStripLine;                                                   // the strip's rule
    p.line3 = kPromptBorder;                                                // the prompt's border under the pointer
    p.text1 = kInk;
    p.text3 = kNotice;
    p.text4 = kPlaceholder;
    p.textSelection = kSelection;
    auto type = [&](PrKitTextRole role, float size, float lineHeight) {
        PrKitType& t = theme.types[role];
        t.mono = false;
        t.italic = false;
        t.size = size;
        t.weight = 400;
        t.lineHeight = lineHeight;
        t.letterSpacing = 0;
    };
    type(PrKitTextRole_Body, kBodySize, kLinePitch);                 // transcript, prompt, tabs
    type(PrKitTextRole_BodySmall, kLabelSize, 1.2f * kLabelSize);    // the model label
    type(PrKitTextRole_Meta, kMetaSize, 1.3f * kMetaSize);           // "queued"
    return theme;
}

void buildWindow(View& view, PrKit* kit, PrLayoutNode parent) {
    Builder b{view, kit, kit->ui, kit->tree, *view.look, *view.model};
    b.build(parent);
}

bool takeEvent(View& view, PrUi* ui, const PrAppEvent& event) {
    Filtered f = filterEvent(view.input, event.kind, event.key, event.code, event.modifiers);
    if (f.action == Action::SelectTab)
        view.model->select_tab(f.tab);
    if (f.consume)
        return true;
    if (f.modifiers != event.modifiers) {
        PrAppEvent copy = event;
        copy.modifiers = f.modifiers;
        prUiPushEvent(ui, &copy);
        return true;
    }
    return false;
}

} // namespace nexus
