#include "sync/hub.h"

#include <algorithm>
#include <memory>
#include <numeric>

#include "sync/client.h"

namespace nexus::sync {

const char* const kReceived = "(received)";

const StoredTab* MemoryStore::find(const std::string& name) const {
    auto it = tabs_.find(name);
    return it == tabs_.end() ? nullptr : &it->second;
}

void MemoryStore::append(const std::string& name, EntryRef entry) {
    tabs_[name].transcript.push_back(std::move(entry));
}

void MemoryStore::enqueue(const std::string& name, std::string prompt) {
    tabs_[name].queue.push_back(std::move(prompt));
}

Hub::Hub(TabStore& store, Limits limits) : store_(store), limits_(limits) {}

namespace {

EntryRef entry(EntryKind kind, std::string text) {
    return std::make_shared<const TabEntry>(TabEntry{kind, std::move(text), true});
}

std::string kib(std::size_t bytes) {
    return std::to_string(bytes / 1024) + " KiB";
}

} // namespace

bool Hub::apply(const Action& action, std::string* why) {
    const StoredTab* tab = store_.find(action.tab);
    if (tab == nullptr && store_.size() >= limits_.tabs) {
        if (why != nullptr)
            *why = "no tab " + action.tab + " made: the server holds " + std::to_string(limits_.tabs) +
                " tabs, its limit";
        return false;
    }
    std::string refused;
    if (blankText(action.text)) {
        refused = std::string("An empty prompt was not ") + (action.kind == ActionKind::Send ? "sent." : "queued.");
    } else if (action.text.size() > limits_.text) {
        refused = "A prompt of " + std::to_string(action.text.size()) + " bytes was not " +
            (action.kind == ActionKind::Send ? "sent" : "queued") + ": the limit is " + kib(limits_.text) + ".";
    } else if (action.kind == ActionKind::Queue && tab != nullptr) {
        const std::size_t bytes = std::accumulate(tab->queue.begin(), tab->queue.end(), std::size_t(0),
            [](std::size_t n, const std::string& q) { return n + q.size(); });
        if (tab->queue.size() >= limits_.queue)
            refused = "A prompt was not queued: the queue holds " + std::to_string(limits_.queue) +
                " prompts, its limit.";
        else if (bytes + action.text.size() > limits_.queueBytes)
            refused = "A prompt was not queued: the queue would hold more than " + kib(limits_.queueBytes) + ".";
    }
    if (!refused.empty()) {
        store_.append(action.tab, entry(EntryKind::Notice, refused));
        refresh(action.tab);
        if (why != nullptr)
            *why = "tab " + action.tab + ": " + refused;
        return false;
    }
    if (action.kind == ActionKind::Send) {
        store_.append(action.tab, entry(EntryKind::Person, action.text));
        store_.append(action.tab, entry(EntryKind::Friend, kReceived));
    } else {
        store_.enqueue(action.tab, action.text);
    }
    refresh(action.tab);
    return true;
}

// The tab's new tail, in place of its old one; every other tab is shared with the state before.
void Hub::refresh(const std::string& name) {
    const StoredTab* stored = store_.find(name);
    auto fresh = std::make_shared<const TabView>(tailOf(name, stored->transcript, stored->queue, limits_.tail));
    std::vector<TabRef> tabs = state_.tabs();
    auto it = std::lower_bound(tabs.begin(), tabs.end(), name,
        [](const TabRef& t, const std::string& n) { return t->name < n; });
    if (it != tabs.end() && (*it)->name == name)
        *it = fresh;
    else
        tabs.insert(it, fresh);
    state_ = Tabs(std::move(tabs));
}

} // namespace nexus::sync
