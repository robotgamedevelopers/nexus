#include "sync/hub.h"

#include <algorithm>
#include <memory>
#include <numeric>

#include "sync/client.h"

namespace nexus::sync {

const char* const kReceived = "(received)";

TabChange takeQueued() {
    return TabChange{nullptr, {}, std::nullopt, true};
}

const StoredTab* MemoryStore::find(const std::string& name) const {
    auto it = tabs_.find(name);
    return it == tabs_.end() ? nullptr : &it->second;
}

std::vector<std::string> MemoryStore::names() const {
    std::vector<std::string> out;
    for (const auto& [name, tab] : tabs_) {
        out.push_back(name);
    }
    return out;
}

bool MemoryStore::commit(const std::string& name, const std::vector<TabChange>& changes, const std::string& client,
                         std::uint64_t num, std::string* why) {
    if (!client.empty() && num != applied(client) + 1) {
        if (why != nullptr) {
            *why = "client " + client + ": action " + std::to_string(num) + " after " + std::to_string(applied(client));
        }
        return false;
    }
    // Every take must find a prompt in the queue at its point of the step, as the durable store requires.
    const StoredTab* had = find(name);
    std::size_t queued = had == nullptr ? 0 : had->queue.size();
    for (const TabChange& c : changes) {
        if (c.take && queued == 0) {
            if (why != nullptr) {
                *why = "tab " + name + ": a take from a queue with nothing left in it";
            }
            return false;
        }
        if (c.take) {
            --queued;
        } else if (!c.driver && !c.entry) {
            ++queued;
        }
    }
    if (!changes.empty()) {
        StoredTab& t = tabs_[name];
        for (const TabChange& c : changes) {
            if (c.take) {
                t.queue.erase(t.queue.begin());
            } else if (c.driver) {
                t.driver = *c.driver;
            } else if (c.entry) {
                t.transcript.push_back(c.entry);
            } else {
                t.queue.push_back(c.prompt);
            }
        }
    }
    if (!client.empty()) {
        applied_[client] = num;
    }
    return true;
}

std::uint64_t MemoryStore::applied(const std::string& client) const {
    auto it = applied_.find(client);
    return it == applied_.end() ? 0 : it->second;
}

Hub::Hub(TabStore& store, Limits limits, Responder* responder)
    : store_(store), limits_(limits), responder_(responder) {
    for (const std::string& name : store_.names()) {
        refresh(name);
    }
}

namespace {

EntryRef entry(EntryKind kind, std::string text) {
    return std::make_shared<const TabEntry>(TabEntry{kind, std::move(text), true});
}

std::string kib(std::size_t bytes) {
    return std::to_string(bytes / 1024) + " KiB";
}

const char* verb(ActionKind kind) {
    switch (kind) {
        case ActionKind::Send: return "sent";
        case ActionKind::Queue: return "queued";
        case ActionKind::Stop: return "applied";
    }
    return "applied";
}

} // namespace

bool Hub::apply(const Action& action, std::string* why, const std::string& client) {
    std::string failed;
    auto commit = [&](const std::string& name, const std::vector<TabChange>& changes) {
        if (changes.empty() && client.empty()) {
            return true;    // nothing to change and no counter to move
        }
        if (store_.commit(name, changes, client, action.num, &failed)) {
            return true;
        }
        if (why != nullptr) {
            *why = "tab " + action.tab + ": the action was not applied: " + failed;
        }
        return false;
    };
    const StoredTab* tab = store_.find(action.tab);
    if (action.kind == ActionKind::Stop && tab == nullptr) {
        // Nothing runs in a tab the server does not hold, and a stop makes no tab: refused, and counted.
        if (!commit(std::string(), {})) {
            return false;
        }
        if (why != nullptr) {
            *why = "no tab " + action.tab + ": nothing to stop";
        }
        return false;
    }
    if (tab == nullptr && store_.size() >= limits_.tabs) {
        // Refused, and counted: the client's counter moves, so the action is never taken again.
        if (!commit(std::string(), {})) {
            return false;
        }
        if (why != nullptr) {
            *why = "no tab " + action.tab + " made: the server holds " + std::to_string(limits_.tabs) +
                " tabs, its limit";
        }
        return false;
    }
    std::string refused;
    if (action.kind == ActionKind::Stop) {
        if (!action.text.empty()) {
            refused = "A stop that carried text was not applied: a stop carries none.";
        }
    } else if (blankText(action.text)) {
        refused = std::string("An empty prompt was not ") + verb(action.kind) + ".";
    } else if (action.text.size() > limits_.text) {
        refused = "A prompt of " + std::to_string(action.text.size()) + " bytes was not " + verb(action.kind) +
            ": the limit is " + kib(limits_.text) + ".";
    } else if (action.kind == ActionKind::Queue && tab != nullptr) {
        const std::size_t bytes = std::accumulate(tab->queue.begin(), tab->queue.end(), std::size_t(0),
            [](std::size_t n, const std::string& q) { return n + q.size(); });
        if (tab->queue.size() >= limits_.queue) {
            refused = "A prompt was not queued: the queue holds " + std::to_string(limits_.queue) +
                " prompts, its limit.";
        } else if (bytes + action.text.size() > limits_.queueBytes) {
            refused = "A prompt was not queued: the queue would hold more than " + kib(limits_.queueBytes) + ".";
        }
    }
    if (!refused.empty()) {
        if (!commit(action.tab, {TabChange{entry(EntryKind::Notice, refused), {}}})) {
            return false;
        }
        refresh(action.tab);
        if (why != nullptr) {
            *why = "tab " + action.tab + ": " + refused;
        }
        return false;
    }
    std::vector<TabChange> changes;
    if (action.kind == ActionKind::Send && responder_ != nullptr) {
        changes = responder_->send(action.tab, action.text);
    } else if (action.kind == ActionKind::Send) {
        changes.push_back(TabChange{entry(EntryKind::Person, action.text), {}});
        changes.push_back(TabChange{entry(EntryKind::Friend, kReceived), {}});
    } else if (action.kind == ActionKind::Queue && responder_ != nullptr) {
        changes = responder_->queue(action.tab, action.text);
    } else if (action.kind == ActionKind::Queue) {
        changes.push_back(TabChange{nullptr, action.text});
    } else if (responder_ != nullptr) {
        changes = responder_->stop(action.tab);
    }
    const bool committed = commit(action.tab, changes);
    if (responder_ != nullptr) {
        responder_->sent(action.tab, committed);
    }
    if (!committed) {
        return false;
    }
    if (store_.find(action.tab) != nullptr) {
        refresh(action.tab);
    }
    return true;
}

bool Hub::commit(const std::string& tab, const std::vector<TabChange>& changes, std::string* why) {
    if (changes.empty()) {
        return true;
    }
    if (!store_.commit(tab, changes, std::string(), 0, why)) {
        return false;
    }
    refresh(tab);
    return true;
}

bool Hub::setOpen(const std::string& tab, const std::string& text) {
    if (store_.find(tab) == nullptr || text.size() > limits_.text) {
        return false;
    }
    auto it = open_.find(tab);
    if (text.empty()) {
        if (it == open_.end()) {
            return false;
        }
        open_.erase(it);
    } else if (it != open_.end() && it->second->text == text) {
        return false;
    } else {
        open_[tab] = std::make_shared<const TabEntry>(TabEntry{EntryKind::Friend, text, false});
    }
    refresh(tab);
    return true;
}

// The tab's new tail, in place of its old one; every other tab is shared with the state before.
void Hub::refresh(const std::string& name) {
    const StoredTab* stored = store_.find(name);
    auto open = open_.find(name);
    auto fresh = std::make_shared<const TabView>(tailOf(name, stored->transcript, stored->queue, limits_.tail,
        open == open_.end() ? nullptr : open->second));
    std::vector<TabRef> tabs = state_.tabs();
    auto it = std::lower_bound(tabs.begin(), tabs.end(), name,
        [](const TabRef& t, const std::string& n) { return t->name < n; });
    if (it != tabs.end() && (*it)->name == name) {
        *it = fresh;
    } else {
        tabs.insert(it, fresh);
    }
    state_ = Tabs(std::move(tabs));
}

} // namespace nexus::sync
