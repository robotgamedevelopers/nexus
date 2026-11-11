#include "server/durable.h"

namespace nexus::server {

namespace {

sync::EntryRef toRef(const store::Entry& e) {
    return std::make_shared<const sync::TabEntry>(
        sync::TabEntry{static_cast<EntryKind>(e.kind), e.text, e.closed});
}

sync::DriverState toState(const store::Driver& d) {
    return sync::DriverState{d.next_turn, d.open_turn, d.session, d.waiting};
}

store::Driver toDriver(const sync::DriverState& d) {
    return store::Driver{d.nextTurn, d.openTurn, d.session, d.waiting};
}

} // namespace

bool DurableStore::open(store::Fs& fs, const std::string& dir, std::string* why, const store::StoreOptions& options) {
    tabs_.clear();
    store::Status s = store::Store::open(fs, dir, options, store_, &report_);
    if (!s.is_ok()) {
        store_.reset();
        if (why != nullptr)
            *why = s.message();
        return false;
    }
    for (const auto& [name, tab] : store_->tabs()) {
        sync::StoredTab& t = tabs_[name];
        for (const store::Entry& e : tab.transcript)
            t.transcript.push_back(toRef(e));
        t.queue = tab.queue;
        t.driver = toState(tab.driver);
    }
    return true;
}

const sync::StoredTab* DurableStore::find(const std::string& name) const {
    auto it = tabs_.find(name);
    return it == tabs_.end() ? nullptr : &it->second;
}

std::vector<std::string> DurableStore::names() const {
    std::vector<std::string> out;
    for (const auto& [name, tab] : tabs_)
        out.push_back(name);
    return out;
}

bool DurableStore::commit(const std::string& name, const std::vector<sync::TabChange>& changes,
                          const std::string& client, std::uint64_t num, std::string* why) {
    if (store_ == nullptr) {
        if (why != nullptr)
            *why = "the store is not open";
        return false;
    }
    std::vector<store::Change> out;
    out.reserve(changes.size());
    for (const sync::TabChange& c : changes) {
        store::Change sc;
        if (c.driver) {
            sc.kind = store::Change::Kind::SetDriver;
            sc.driver = toDriver(*c.driver);
        } else if (c.entry) {
            sc.kind = store::Change::Kind::Append;
            sc.entry = store::Entry{static_cast<std::uint8_t>(c.entry->kind), c.entry->text, c.entry->closed};
        } else {
            sc.kind = store::Change::Kind::Enqueue;
            sc.prompt = c.prompt;
        }
        out.push_back(std::move(sc));
    }
    store::Status s = store_->commit(name, out, client, num);
    if (!s.is_ok()) {
        if (why != nullptr)
            *why = s.message();
        return false;
    }
    if (!changes.empty()) {
        sync::StoredTab& t = tabs_[name];
        for (const sync::TabChange& c : changes) {
            if (c.driver)
                t.driver = *c.driver;
            else if (c.entry)
                t.transcript.push_back(c.entry);
            else
                t.queue.push_back(c.prompt);
        }
    }
    return true;
}

std::uint64_t DurableStore::applied(const std::string& client) const {
    return store_ == nullptr ? 0 : store_->applied(client);
}

} // namespace nexus::server
