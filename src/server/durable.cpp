#include "server/durable.h"

namespace nexus::server {

namespace {

// The store keeps an entry's kind as it was written; a kind this build does not know (a later server's) is shown as a
// notice, a plain dim line, and never handed on as a kind the window cannot draw.
sync::EntryRef toRef(const store::Entry& e) {
    const EntryKind kind = e.kind <= static_cast<std::uint8_t>(EntryKind::Tool) ? static_cast<EntryKind>(e.kind) :
        EntryKind::Notice;
    return std::make_shared<const sync::SessionEntry>(sync::SessionEntry{kind, e.text, e.closed});
}

sync::DriverState toState(const store::Driver& d) {
    return sync::DriverState{d.next_turn, d.open_turn, d.session, d.waiting, d.summary, d.session_dir, d.session_account,
        d.continuation_prompt, d.exhausted_accounts, d.model_limit_pending};
}

store::Driver toDriver(const sync::DriverState& d) {
    return store::Driver{d.nextTurn, d.openTurn, d.session, d.waiting, d.summary, d.sessionDir, d.sessionAccount,
        d.continuationPrompt, d.exhaustedAccounts, d.modelLimitPending};
}

sync::SessionInfo toInfo(const store::Info& i) {
    return sync::SessionInfo{i.friend_name, i.harness, i.account, i.model, i.effort, i.instance, i.auto_account};
}

store::Info toStored(const sync::SessionInfo& i) {
    return store::Info{i.friendName, i.harness, i.account, i.model, i.effort, i.instance, i.autoAccount};
}

} // namespace

bool DurableStore::open(store::Fs& fs, const std::string& dir, std::string* why, const store::StoreOptions& options) {
    sessions_.clear();
    busy_ = false;
    // A store this one held is closed first, which releases its lock: an open again is this process's own.
    store_.reset();
    store::Status s = store::Store::open(fs, dir, options, store_, &report_);
    if (!s.is_ok()) {
        store_.reset();
        busy_ = s.code() == store::Status::Code::Busy;
        if (why != nullptr) {
            *why = s.message();
        }
        return false;
    }
    for (const auto& [id, session] : store_->sessions()) {
        sync::StoredSession& t = sessions_[id];
        t.info = toInfo(session.info);
        for (const store::Entry& e : session.transcript) {
            t.transcript.push_back(toRef(e));
        }
        t.queue = session.queue;
        t.driver = toState(session.driver);
    }
    return true;
}

const sync::StoredSession* DurableStore::find(sync::SessionId id) const {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : &it->second;
}

std::vector<sync::SessionId> DurableStore::ids() const {
    std::vector<sync::SessionId> out;
    for (const auto& [id, session] : sessions_) {
        out.push_back(id);
    }
    return out;
}

bool DurableStore::commit(sync::SessionId id, const std::vector<sync::SessionChange>& changes,
                          const std::string& client, std::uint64_t num, std::string* why) {
    if (store_ == nullptr) {
        if (why != nullptr) {
            *why = "the store is not open";
        }
        return false;
    }
    std::vector<store::Change> out;
    out.reserve(changes.size());
    for (const sync::SessionChange& c : changes) {
        store::Change sc;
        if (c.take) {
            sc.kind = store::Change::Kind::Dequeue;
        } else if (c.info) {
            sc.kind = store::Change::Kind::SetInfo;
            sc.info = toStored(*c.info);
        } else if (c.driver) {
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
    store::Status s = store_->commit(id, out, client, num);
    if (!s.is_ok()) {
        if (why != nullptr) {
            *why = s.message();
        }
        return false;
    }
    if (!changes.empty()) {
        sync::StoredSession& t = sessions_[id];     // the store refused the step had it not held or made the session
        for (const sync::SessionChange& c : changes) {
            if (c.take) {
                t.queue.erase(t.queue.begin());     // the store refused the step had the queue nothing left
            } else if (c.info) {
                t.info = *c.info;
            } else if (c.driver) {
                t.driver = *c.driver;
            } else if (c.entry) {
                t.transcript.push_back(c.entry);
            } else {
                t.queue.push_back(c.prompt);
            }
        }
    }
    return true;
}

void DurableStore::beginBatch() {
    if (store_ != nullptr) {
        store_->begin_batch();
    }
}

bool DurableStore::flush(std::string* why) {
    if (store_ == nullptr) {
        if (why != nullptr) {
            *why = "the store is not open";
        }
        return false;
    }
    const store::Status s = store_->flush();
    if (!s.is_ok()) {
        if (why != nullptr) {
            *why = s.message();
        }
        return false;
    }
    return true;
}

std::uint64_t DurableStore::applied(const std::string& client) const {
    return store_ == nullptr ? 0 : store_->applied(client);
}

} // namespace nexus::server
