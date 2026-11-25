#include "chat_selection.h"
#include "style.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nexus {

void ChatSelection::begin(PrLayoutTree* tree, size_t tab) {
    if (tab_ != tab || tree_ != tree) {
        ranges_.clear(); text_.clear(); dragging_ = false;
    }
    tree_ = tree;
    tab_ = tab;
    paragraphs_.clear();
    links_.clear();
}

void ChatSelection::paragraph(PrLayoutNode root, PrColor16 ink) {
    paragraphs_.push_back(root);
    if (ranges_.empty()) return;
    textNodes(tree_, root, [&](PrLayoutNode node) {
        const auto it = std::find_if(ranges_.begin(), ranges_.end(), [&](const Range& range) { return range.node == node; });
        if (it == ranges_.end()) return;
        const auto source = prLayoutNodeText(tree_, node);
        if (std::string_view(source.data, source.size) != it->source) return;
        PrLayoutHighlight h{};
        h.start = it->first; h.end = it->last; h.background = ink;
        prLayoutTreeFrameSetHighlights(tree_, node, &h, 1);
    });
}

void ChatSelection::select(PrLayoutTextPosition end) {
    struct Slice { PrLayoutNode node, paragraph; };
    std::vector<Slice> nodes;
    for (auto p : paragraphs_)
        textNodes(tree_, p, [&](PrLayoutNode n) { nodes.push_back({n,p}); });
    auto locate = [&](PrLayoutNode n) {
        return std::find_if(nodes.begin(), nodes.end(), [&](Slice s) { return s.node == n; });
    };
    auto first = locate(anchor_.node), last = locate(end.node);
    if (first == nodes.end() || last == nodes.end()) return;
    auto start = anchor_.offset, stop = end.offset;
    if (first > last || (first == last && start > stop)) {
        std::swap(first, last); std::swap(start, stop);
    }
    ranges_.clear(); text_.clear();
    PrLayoutNode previous = kPrLayoutNodeNone;
    for (auto it = first; it <= last; ++it) {
        const auto s = prLayoutNodeText(tree_, it->node);
        const uint32_t a = it == first ? std::min<size_t>(start,s.size) : 0;
        const uint32_t b = it == last ? std::min<size_t>(stop,s.size) : s.size;
        if (b > a) {
            if (previous && previous != it->paragraph) text_ += "\n\n";
            previous = it->paragraph;
            ranges_.push_back({it->node,a,b,std::string(s.data,s.size)});
            text_.append(s.data+a,b-a);
        }
    }
}

void ChatSelection::link(PrLayoutNode leaf, const std::string& url) {
    links_.push_back({leaf, url});
}

std::string ChatSelection::linkAt(PrVec2 point, PrLayoutNode leaf) const {
    if (!leaf || links_.empty()) return {};
    for (const auto& link : links_) {
        if (link.leaf != leaf) continue;
        // Hit actual glyph lines. The empty space after a wrapped link belongs to the paragraph.
        const auto length = prLayoutNodeText(tree_, leaf).size;
        const auto coords = PrLayoutCoordinates_Viewport;
        std::vector<PrLayoutRect> boxes(prLayoutNodeTextRangeBoxes(tree_, leaf, 0, length, coords, nullptr, 0));
        prLayoutNodeTextRangeBoxes(tree_, leaf, 0, length, coords, boxes.data(), boxes.size());
        for (const auto box : boxes) {
            const auto r = prLayoutRectToPx(box);
            if (point.x >= r.left && point.x < r.right && point.y >= r.top && point.y < r.bottom) return link.url;
        }
    }
    return {};
}

bool ChatSelection::event(const PrAppEvent& e, PrLayoutNode viewport, const std::function<bool(const std::string&)>& copy,
                          const std::function<void(const std::string&)>& open) {
    if (paragraphs_.empty()) return false;
    const auto mods = e.modifiers & ~PrAppModifierFlag_CapsLock;
    if (e.kind == PrAppEventKind_KeyDown && e.keyboard.key == PrAppKey('C') &&
        (mods == PrAppModifierFlag_Control || mods == PrAppModifierFlag_Super || mods == style::kCommand) && !text_.empty() && copy) {
        if (copy(text_)) {
            ranges_.clear();
            text_.clear();
            dragging_ = false;
        }
        return true;
    }
    if (e.kind == PrAppEventKind_Text || e.kind == PrAppEventKind_Paste) {
        ranges_.clear();
        text_.clear();
        dragging_ = false;
    }
    if (e.kind == PrAppEventKind_PointerCancel) { dragging_ = false; return false; }
    const bool down = e.kind == PrAppEventKind_PointerDown && e.pointer.button == 0;
    if (!down && !(dragging_ && (e.kind == PrAppEventKind_PointerMove || e.kind == PrAppEventKind_PointerUp))) return false;
    if (down) { dragging_ = false; ranges_.clear(); text_.clear(); }
    const auto pos = e.pointer.position;
    if (down) {
        if (!prLayoutNodeIsValid(tree_, viewport)) return false;
        const auto area = prLayoutRectToPx(prLayoutNodeBounds(tree_, viewport, PrLayoutCoordinates_Viewport));
        if (pos.x < area.left || pos.x >= area.right || pos.y < area.top || pos.y >= area.bottom) return false;
    }
    float nearestLine = std::numeric_limits<float>::infinity();
    float nearestColumn = std::numeric_limits<float>::infinity();
    PrLayoutTextPosition hit{};
    for (auto p : paragraphs_) {
        auto r = prLayoutRectToPx(prLayoutNodeBounds(tree_, p, PrLayoutCoordinates_Viewport));
        PrLayoutTextPosition first{}, last{};
        textNodes(tree_, p, [&](PrLayoutNode leaf) {
            const auto length = prLayoutNodeText(tree_, leaf).size;
            if (!length) return;
            const auto bounds = prLayoutRectToPx(prLayoutNodeBounds(tree_, leaf, PrLayoutCoordinates_Viewport));
            if (!first.node) {
                first = {leaf, 0};
                r.top = bounds.top;
                r.bottom = bounds.bottom;
            } else {
                r.top = std::min(r.top, bounds.top);
                r.bottom = std::max(r.bottom, bounds.bottom);
            }
            last = {leaf, static_cast<uint32_t>(length)};
        });
        if (!first.node) continue;
        const float dx = std::max({r.left-pos.x, pos.x-r.right, 0.0f});
        const float dy = std::max({r.top-pos.y, pos.y-r.bottom, 0.0f});
        if (dy > nearestLine || (dy == nearestLine && dx >= nearestColumn)) continue;
        PrLayoutTextPosition candidate{};
        if (pos.y < r.top || pos.y >= r.bottom) {
            candidate = pos.y < r.top ? first : last;
        } else {
            prLayoutTextPositionFromPoint(tree_, p, {prLayoutUnitFromPx(pos.x),prLayoutUnitFromPx(pos.y)}, &candidate);
        }
        if (candidate.node) {
            nearestLine = dy; nearestColumn = dx; hit = candidate;
        }
    }
    if (down && !hit.node) return false;
    if (down) {
        anchor_ = hit;
        dragging_ = true;
        pressedAt_ = pos;
        pressedLink_ = linkAt(pos, hit.node);
        if (e.pointer.clicks == 2 && nearestLine == 0 && nearestColumn == 0 && hit.node) {
            const auto raw = prLayoutNodeText(tree_, hit.node);
            std::string_view word(raw.data, raw.size);
            auto end = hit;
            const auto space = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
            while (anchor_.offset && !space(word[anchor_.offset - 1])) --anchor_.offset;
            while (end.offset < word.size() && !space(word[end.offset])) ++end.offset;
            select(end);
            pressedLink_.clear();
            dragging_ = false;
            return true;
        }
    }
    if (std::abs(pos.x - pressedAt_.x) > 3 || std::abs(pos.y - pressedAt_.y) > 3) pressedLink_.clear();
    if (hit.node) select(hit);
    if (e.kind == PrAppEventKind_PointerUp) {
        dragging_ = false;
        if (open && !pressedLink_.empty() && text_.empty() && linkAt(pos, hit.node) == pressedLink_)
            open(pressedLink_);
        pressedLink_.clear();
    }
    return true;
}
} // namespace nexus
