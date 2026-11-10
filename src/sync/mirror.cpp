#include "sync/mirror.h"

namespace nexus::sync {

std::string notShownNotice(std::uint64_t count) {
    return std::to_string(count) + (count == 1 ? " earlier entry not shown" : " earlier entries not shown");
}

Mirror::Seen Mirror::seen(std::size_t id) const {
    auto it = seen_.find(id);
    return it == seen_.end() ? Seen{} : it->second;
}

bool Mirror::apply(Model& model, const Tabs& state) {
    bool changed = false;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        const Tab& tab = model.tab(i);
        if (tab.kind != TabKind::Friend)
            continue;
        const TabView* view = state.find(tab.name);
        if (view == nullptr)
            continue;
        Seen& seen = seen_[tab.id];
        if (view->first > seen.next) {
            model.append(i, Entry{EntryKind::Notice, notShownNotice(view->first - seen.next)});
            seen.next = view->first;
            changed = true;
        }
        while (seen.next < view->total()) {
            const TabEntry& e = *view->entries[static_cast<std::size_t>(seen.next - view->first)];
            if (!e.closed)
                break;
            model.append(i, Entry{e.kind, e.text});
            ++seen.next;
            changed = true;
        }
        // The model's one way into a queue is queue(), which takes the tab's draft: so the person's draft is put aside,
        // each prompt queued through the draft, and the draft put back. All of it happens between two rounds of the
        // window, so the prompt never shows the queued text.
        if (seen.queued < view->queue.size()) {
            const std::string draft = tab.draft;
            for (; seen.queued < view->queue.size(); ++seen.queued) {
                model.set_draft(i, view->queue[seen.queued]);
                model.queue(i);
            }
            model.set_draft(i, draft);
            changed = true;
        }
    }
    return changed;
}

} // namespace nexus::sync
