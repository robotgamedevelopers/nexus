#include "sync/mirror.h"

#include <algorithm>
#include <vector>

namespace nexus::sync {

namespace {

// How many of `mine`'s oldest prompts the server took from the queue, given that its queue is now `theirs`: the fewest
// after which the rest of `mine` is where `theirs` starts. The server's queue changes only by taking from its head and
// putting at its end, so the rest of `mine` is always a start of `theirs` for some count (all of it, at worst), and
// any count that fits leaves the same queue.
std::size_t takenFrom(const std::vector<std::string>& mine, const std::vector<std::string>& theirs) {
    std::size_t k = 0;
    while (k < mine.size() &&
           (mine.size() - k > theirs.size() || !std::equal(mine.begin() + static_cast<std::ptrdiff_t>(k), mine.end(),
                                                           theirs.begin()))) {
        ++k;
    }
    return k;
}

} // namespace

std::string notShownNotice(std::uint64_t count) {
    return std::to_string(count) + (count == 1 ? " earlier entry not shown" : " earlier entries not shown");
}

SessionId Mirror::session(std::size_t id) const {
    auto it = session_.find(id);
    return it == session_.end() ? 0 : it->second;
}

Mirror::Seen Mirror::seen(std::size_t id) const {
    auto it = seen_.find(id);
    return it == seen_.end() ? Seen{} : Seen{it->second.next};
}

const SessionEntry* Mirror::open(std::size_t id) const {
    auto it = seen_.find(id);
    return it == seen_.end() ? nullptr : it->second.open.get();
}

std::uint32_t Mirror::activeAgents(std::size_t id) const {
    auto it = seen_.find(id);
    return it == seen_.end() || !it->second.held ? 0 : it->second.agents;
}

const Activity* Mirror::activity(std::size_t id) const {
    auto it = seen_.find(id);
    return it == seen_.end() || !it->second.held ? nullptr : &it->second.activity;
}

void Mirror::clearOpen(std::size_t tabId) {
    auto it = seen_.find(tabId);
    if (it != seen_.end()) {
        // The window just cleared this transcript locally. A future roster change must not bring it back.
        it->second.missingHistory = false;
        it->second.bus.clear();
        it->second.visibleBus = 0;
        if (it->second.open) {
            it->second.open.reset();
            ++it->second.next;
        }
    }
}

void Mirror::forget(std::size_t id) {
    session_.erase(id);
    seen_.erase(id);
}

bool Mirror::reconcileBus(Model& model, std::size_t tab, Shown& seen, const SessionView& view) {
    const auto& transcript = model.tab(tab).transcript;
    std::vector<TranscriptEdit> edits;
    std::size_t oldVisible = 0;
    for (auto& slot : seen.bus) {
        const std::size_t at = slot.ordinaryBefore + oldVisible;
        oldVisible += slot.visible;
        const SessionEntry* tail = nullptr;
        if (slot.rawIndex >= view.first && slot.rawIndex < view.total())
            tail = view.entries[static_cast<std::size_t>(slot.rawIndex - view.first)].get();
        const Entry* cached = slot.visible ? &transcript[at] : slot.withdrawn.get();
        const bool visible = tail ? tail->kind == EntryKind::Bus && tail->closed && visibleBus(tail->text, view)
                                  : cached && (slot.visible || view.busSenders) && visibleBus(cached->text, view);
        if (visible == slot.visible && (!visible || !tail || tail->text == transcript[at].text)) continue;
        TranscriptEdit edit{at, slot.visible, std::nullopt};
        if (visible) {
            if (tail) edit.replacement = Entry{EntryKind::Bus, tail->text, tail->accountEmailEnd};
            else edit.replacement = std::move(*slot.withdrawn);
            slot.withdrawn.reset();
        } else {
            slot.withdrawn = std::make_unique<Entry>();
            edit.removed = slot.withdrawn.get();
        }
        edits.push_back(std::move(edit));
        if (visible != slot.visible) {
            if (visible) ++seen.visibleBus;
            else --seen.visibleBus;
            slot.visible = visible;
        }
    }
    if (edits.empty()) return false;
    model.edit_transcript(tab, std::move(edits));
    return true;
}

bool Mirror::apply(Model& model, const Sessions& state) {
    bool changed = false;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        const Tab& tab = model.tab(i);
        if (tab.kind != TabKind::Friend) {
            continue;
        }
        const SessionView* view = nullptr;
        auto shows = session_.find(tab.id);
        if (shows != session_.end()) {
            view = state.find(shows->second);
        } else if ((view = state.first(tab.name)) != nullptr) {
            session_[tab.id] = view->id;     // the friend's first session, from now on
        }
        if (view == nullptr) {
            // The session is gone from the state: nothing of it is held, its open entry included, so no text of it
            // grows beside a cursor that says the window holds nothing.
            auto had = seen_.find(tab.id);
            if (had != seen_.end() && (had->second.held || had->second.open)) {
                had->second.held = false;
                had->second.open = nullptr;
                changed = true;
            }
            continue;
        }
        Shown& seen = seen_[tab.id];
        if (seen.epoch != view->info.contextEpoch || (seen.missingHistory && view->first == 0)) {
            model.clear_transcript(i);
            seen = {};
            seen.epoch = view->info.contextEpoch;
            changed = true;
        }
        // A returning or expanded tail can supply bodies we dropped earlier. Usual appends and streaming updates
        // leave all earlier slots alone.
        if (!seen.held || seen.busSenders != view->busSenders || view->first < seen.tailFirst) {
            if (reconcileBus(model, i, seen, *view)) changed = true;
            seen.busSenders = view->busSenders;
        }
        seen.tailFirst = view->first;
        if (!seen.held || seen.activity != view->activity || seen.agents != view->activeAgents) {
            seen.held = true;
            seen.activity = view->activity;
            seen.agents = view->activeAgents;
            changed = true;
        }
        if (view->first > seen.next) {
            seen.missingHistory = true;
            model.append(i, Entry{EntryKind::Notice, notShownNotice(view->first - seen.next)});
            seen.next = view->first;
            changed = true;
        }
        while (seen.next < view->total()) {
            const SessionEntry& e = *view->entries[static_cast<std::size_t>(seen.next - view->first)];
            if (!e.closed) {
                break;
            }
            if (e.kind == EntryKind::Bus) {
                const bool visible = visibleBus(e.text, *view);
                seen.bus.push_back({seen.next, tab.transcript.size() - seen.visibleBus, visible});
                if (!visible) {
                    ++seen.next;
                    continue;
                }
                ++seen.visibleBus;
            }
            // Driver lifecycle diagnostics remain in server history, including on older servers.
            if (e.kind == EntryKind::Notice && e.text.starts_with(tab.name + "'s driver was ended by signal ")) {
                ++seen.next;
                continue;
            }
            model.append(i, Entry{e.kind, e.text, e.accountEmailEnd});
            ++seen.next;
            changed = true;
        }
        // What stopped the loop, if anything, is the entry still open.
        EntryRef open = seen.next < view->total() ? view->entries[static_cast<std::size_t>(seen.next - view->first)]
                                                  : nullptr;
        if (!(open == seen.open || (open && seen.open && *open == *seen.open))) {
            seen.open = std::move(open);
            changed = true;
        }
        // The queue: what the server took from its head leaves the model's, then the prompts queued since are put at
        // its end. The model's one way into a queue is queue(), which takes the tab's draft: so the person's draft is
        // put aside, each prompt queued through the draft, and the draft put back. All of it happens between two
        // rounds of the window, so the prompt never shows the queued text.
        const std::vector<std::string> mine = tab.queue;
        const std::size_t taken = takenFrom(mine, view->queue);
        for (std::size_t k = 0; k < taken; ++k) {
            model.take_queued(i);
            changed = true;
        }
        if (mine.size() - taken < view->queue.size()) {
            const std::string draft = tab.draft;
            for (std::size_t q = mine.size() - taken; q < view->queue.size(); ++q) {
                model.set_draft(i, view->queue[q]);
                model.queue(i);
            }
            model.set_draft(i, draft);
            changed = true;
        }
    }
    return changed;
}

} // namespace nexus::sync
