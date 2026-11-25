#pragma once

#include "pr/app_event.h"
#include "pr/layout_tree.h"

#include <functional>
#include <string>
#include <vector>

namespace nexus {

template<class F> void textNodes(PrLayoutTree* tree, PrLayoutNode root, const F& fn) {
    if (prLayoutNodeKind(tree, root) == PrLayoutNodeKind_Text) {
        fn(root);
        return;
    }
    for (auto c = prLayoutNodeFirstChild(tree, root); c != kPrLayoutNodeNone; c = prLayoutNodeNextSibling(tree, c))
        textNodes(tree, c, fn);
}

class ChatSelection {
public:
    void begin(PrLayoutTree* tree, size_t tab);
    void paragraph(PrLayoutNode root, PrColor16 ink);
    void link(PrLayoutNode leaf, const std::string& url);
    bool event(const PrAppEvent& event, PrLayoutNode viewport, const std::function<bool(const std::string&)>& copy,
               const std::function<void(const std::string&)>& open = {});
private:
    struct Link { PrLayoutNode leaf; std::string url; };
    std::vector<Link> links_;
    std::string pressedLink_;
    PrVec2 pressedAt_{};
    std::string linkAt(PrVec2 point, PrLayoutNode leaf) const;
    struct Range { PrLayoutNode node; uint32_t first, last; std::string source; };
    PrLayoutTree* tree_ = nullptr;
    size_t tab_ = 0;
    std::vector<PrLayoutNode> paragraphs_;
    std::vector<Range> ranges_;
    PrLayoutTextPosition anchor_{};
    bool dragging_ = false;
    std::string text_;
    void select(PrLayoutTextPosition end);
};

} // namespace nexus
