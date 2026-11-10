#include "view.h"

#include <algorithm>
#include <cmath>
#include <string_view>

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
    LabelRow,
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
                padding(box, kU * k, 0, 0, 0);
                break;
            case Part::Prompt:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kPromptPercent);
                padding(box, promptPadY(k), kPromptPadX * k, promptPadY(k), kPromptPadX * k);
                for (PrLayoutUnit& b : box.border)
                    b = prLayoutUnitFromPx(1);
                break;
            case Part::LabelRow:
                box.display = PrLayoutDisplay_Flex;
                box.width = prLayoutLengthPercent(kPromptPercent);
                box.height = px(kPromptBottom * k);
                padding(box, 0, 0.7f * kU * k, (kLabelBaseline - 0.32f * kLabelSize) * k, 0);
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
                // Reversed, the scroller's origin is its bottom (css-overflow-3 scroll origin): it starts at the newest
                // entry and stays there as entries arrive, and scroll anchoring holds a view scrolled up in place.
                flex.direction = PrLayoutFlexDirection_ColumnReverse;
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
    signed char s = static_cast<signed char>(look.textSteps);
    return pr::uiStyle(ui, look, BoxRecipe{p, s}, DecorationRecipe{p, s}, FlexRecipe{p, s});
}

// ---------------------------------------------------------------------------------------------------------------------
// The tree.

PrStringView sv(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

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

    // Lets the text under `node` break anywhere when nothing else breaks it (CSS overflow-wrap: anywhere), so a long URL
    // or token wraps inside its box instead of running past it. Prax's layout has the option (PrLayoutFontStyle
    // overflowWrap); the kit offers none (PrKitTextStyle wrap is wrap, nowrap or clip, and PrKitTextArea has no wrap
    // option), so the node's font group is rewritten, as frameless rewrites the decoration.
    void breakAnywhere(PrLayoutNode node) {
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, node));
        PrLayoutFontStyle font = prLayoutFontStyleRecord(tree, record.groups[PrLayoutGroup_Font]);
        font.overflowWrap = PrTextOverflowWrap_Anywhere;
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &font);
        prLayoutTreeFrameSetStyle(tree, node, prLayoutStyleIntern(tree, &record));
    }

    // A transcript's text: a kit label that breaks anywhere.
    PrLayoutNode text(PrLayoutNode parent, std::string_view s, PrKitTextStyle style) {
        PrLayoutNode node = label(parent, s, style);
        breakAnywhere(node);
        return node;
    }

    // The kit's reorderable tab strip: a tab pressed and dragged past the kit's threshold is carried, an accent caret
    // marks where it will land, Escape cancels it, and the drop is reported by prKitMoved, which the model applies. A
    // press released before the threshold is a click that selects the tab. Each tab is keyed by its friend, so its node
    // (focus, pointer state) goes with it when it moves.
    void strip(PrLayoutNode window) {
        PrLayoutNode row = prKitTabStrip(kit, window, PrKitTabStrip{.key = 0, .disabled = false, .reorderable = true},
            PrKitChoiceStyle{.size = PrKitControlSize_Lg, .direction = PrKitDirection_Row});
        PrKitPlace rowPlace{};
        rowPlace.height = prKitPx(kStripHeight * k() + 1);
        rowPlace.flex = PrKitFlex_Fixed;
        prKitPlace(kit, row, &rowPlace);
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            PrLayoutNode tab = prKitTab(kit, row, PrKitTab{.key = prUiKeyIndex(ui, row, model.tab(i).id),
                .selected = i == model.active(), .disabled = false});
            PrKitPlace tabPlace{};
            tabPlace.height = prKitPx(1.75f * kTabSize * k());
            tabPlace.align = PrKitAlign_Center;
            if (i == 0)
                tabPlace.margin.left = 0.8f * kU * k();
            prKitPlace(kit, tab, &tabPlace);
            if (prKitActivated(kit, tab))
                model.select_tab(i);
            prKitTabLabel(kit, tab, PrKitLabel{.key = 0, .text = sv(label(model.tab(i)))});
        }
        // A drop across zones (a terminal among the friends, or a friend among the terminals) is refused by the model
        // and changes nothing: the kit's strip has no way to keep its caret out of another zone.
        uint32_t from = 0;
        uint32_t to = 0;
        if (prKitMoved(kit, row, &from, &to) && model.move_tab(from, to) && view.moved)
            view.moved();
    }

    // A friend's tab shows the friend's name; a terminal's the title its shell set, or "terminal".
    std::string label(const Tab& tab) {
        if (tab.kind == TabKind::Friend)
            return tab.name;
        std::string title = view.terminals != nullptr ? view.terminals->title(tab.id) : std::string();
        return title.empty() ? std::string("terminal") : title;
    }

    // Each friend keeps its scroll while another shows, wherever its tab stands. Offsets count from the scroller's
    // origin, its bottom, so a tab never scrolled stays at its newest entry.
    void keepScroll(PrLayoutNode scroller, std::size_t id) {
        if (view.shown == id)
            return;
        leaveTranscript();
        view.shown = id;
        prLayoutNodeSetScroll(tree, scroller, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(view.scrolls[id])});
    }

    // Keeps the scroll of the friend the transcript showed last round (by Tab::id; the friends' ids are 0 to the number
    // of friends less one, below tab_count()).
    void leaveTranscript() {
        view.scrolls.resize(model.tab_count(), 0);
        if (view.shown < view.scrolls.size() && view.transcript != kPrLayoutNodeNone)
            view.scrolls[view.shown] = prLayoutUnitToPx(prLayoutNodeScroll(tree, view.transcript).y);
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
        const float below = std::max(0.0f, view.extent.y - (kStripHeight * k() + 1));
        view.terminals->show(tab.id, window, PrVec2{view.extent.x, below});
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
        keepScroll(scroller, tab.id);
        view.transcript = scroller;
        for (const Entry& e : tab.transcript) {
            switch (e.kind) {
                case EntryKind::Person: {
                    PrLayoutNode entry = element(column, Part::Entry);
                    PrLayoutNode bubble = element(entry, Part::Bubble);
                    text(bubble, e.text, textStyle(PrKitTextRole_Body, PrKitTone_Primary));
                    break;
                }
                case EntryKind::Friend:
                    text(column, e.text, textStyle(PrKitTextRole_Body, PrKitTone_Primary));
                    break;
                case EntryKind::Notice:
                    text(column, e.text, textStyle(PrKitTextRole_Body, PrKitTone_Muted));
                    break;
            }
        }
        // The tab's queue, after its transcript and apart from it: each prompt held for later, oldest first, in a
        // dimmer bubble marked "queued".
        for (const std::string& text : tab.queue) {
            PrLayoutNode entry = element(column, Part::Entry);
            PrLayoutNode bubble = element(entry, Part::BubbleQueued);
            this->text(bubble, text, textStyle(PrKitTextRole_Body, PrKitTone_Muted));
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
        // The area's text is the element the kit makes as its first child (kit_input.cpp editedText), which carries its
        // own font group, so it is restyled there. No public call names that element: it is found in the last round's
        // tree, whose node this round continues (pr/layout_frame.h), and only if this round created it under the area.
        PrLayoutNode value = prLayoutNodeFirstChild(tree, edit);
        if (value != kPrLayoutNodeNone && prLayoutTreeFrameParent(tree, value) == edit)
            breakAnywhere(value);
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
        // A friend's draft comes back with the caret at its end.
        if (view.prompted != model.tab(active).id) {
            view.prompted = model.tab(active).id;
            uint32_t end = uint32_t(model.tab(active).draft.size());
            prUiTextEditSetSelection(ui, edit, PrUiTextSelection{end, end});
        }

        PrLayoutNode labels = element(footer, Part::LabelRow);
        label(labels, model.tab(active).model, textStyle(PrKitTextRole_BodySmall, PrKitTone_Primary));
    }

    void build(PrLayoutNode parent) {
        PrLayoutNode window = element(parent, Part::Window);
        // Command-1 to Command-0 select the tabs at positions 1 to 10 in the strip's current order, in the keyboard
        // row's order, claimed on the window, an ancestor
        // of the focused prompt. A key whose tab does not exist is refused by the model and changes nothing. macOS
        // types no text for a Command chord.
        for (std::size_t n = 0; n < kTabKeys.size(); ++n) {
            if (prUiKeyPressed(ui, window, PrAppKey(kTabKeys[n]), PrAppModifierFlag_Super))
                model.select_tab(n);
        }
        // The text scale's keys, claimed the same way, ask for a new scale; the next frame applies it (beginWindowFrame).
        for (const TextKey& t : kTextKeys) {
            if (prUiKeyPressed(ui, window, t.key, t.modifiers))
                view.askedTextSteps = std::clamp(view.askedTextSteps + t.steps, -kTextStepsDown, kTextStepsUp);
        }
        // Command-T adds a terminal tab at the end and shows it; Command-W closes the tab that shows if it is a
        // terminal's, and does nothing on a friend's. Claimed on the window like the keys above: a terminal leaves
        // Command chords to the window (terminal.cpp claimKeys), and the prompt claims neither.
        if (prUiKeyPressed(ui, window, PrAppKey('T'), PrAppModifierFlag_Super) && view.terminals != nullptr) {
            const std::size_t id = model.add_terminal();
            if (!view.terminals->open(id))
                model.close_tab(model.active());
        }
        if (prUiKeyPressed(ui, window, PrAppKey('W'), PrAppModifierFlag_Super) && model.tab_count() != 0 &&
                model.tab(model.active()).kind == TabKind::Terminal) {
            const std::size_t id = model.tab(model.active()).id;
            model.close_tab(model.active());
            view.terminals->close(id);
        }
        serviceTerminals();
        // On a terminal tab, Escape and Command-Escape are not claimed: the terminal claims Escape for its shell, and
        // Command-Escape does nothing there.
        const bool onFriend = model.tab_count() == 0 || model.tab(model.active()).kind == TabKind::Friend;
        // While an input method composes, its Enter commits the composition and its Escape cancels it (the KeyDown comes
        // first, then CompositionEnd): read before the area takes this round's events.
        const bool composing = view.prompt != kPrLayoutNodeNone && prUiTextEditIsComposing(ui, view.prompt);
        // Escape hides the overlay (view.hide; unset in --windowed, where it does nothing). It is claimed on the window
        // only while no tab is being dragged: a claim comes before Prax's own Escape, which cancels a drag (ui.cpp
        // routeKey), so during a drag Escape cancels it and hides nothing. While composing it is the input method's.
        if (onFriend && prUiDragging(ui) == nullptr && prUiKeyPressed(ui, window, PrAppKey_Escape, 0) && !composing &&
                view.hide)
            view.hide();
        // Command-Escape is stop (view.stop): nothing runs in this layer that could be stopped; a later layer connects
        // it to stopping the friend's work. It hides nothing. The text area claims neither key, so neither types or edits.
        if (onFriend && prUiKeyPressed(ui, window, PrAppKey_Escape, PrAppModifierFlag_Super) && view.stop)
            view.stop();
        strip(window);
        // the strip may have selected or moved a tab: read the active tab again
        if (model.tab_count() != 0 && model.tab(model.active()).kind == TabKind::Terminal) {
            terminal(window, model.tab(model.active()));
            return;
        }
        transcript(window);
        PrLayoutNode footer = element(window, Part::Footer);
        if (model.tab_count() != 0)
            prompt(footer, composing);
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
    type(PrKitTextRole_BodySmall, kLabelSize, 1.2f * kLabelSize);    // the model label
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
    return true;
}

bool windowWantsFrame(const View& view) {
    return view.look->textSteps != view.askedTextSteps;
}

void buildWindow(View& view, PrKit* kit, PrLayoutNode parent) {
    Builder b{view, kit, kit->ui, kit->tree, *view.look, *view.model};
    b.build(parent);
}

bool takeEvent(PrUi* ui, const PrAppEvent& event) {
    PrAppModifierFlags modifiers = withoutCapsLock(event.modifiers);
    if (modifiers == event.modifiers)
        return false;
    PrAppEvent copy = event;
    copy.modifiers = modifiers;
    prUiPushEvent(ui, &copy);
    return true;
}

} // namespace nexus
