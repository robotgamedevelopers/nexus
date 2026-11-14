#include "sync/hub.h"

#include <algorithm>
#include <memory>
#include <numeric>

#include "sync/client.h"

namespace nexus::sync {

const char* const kReceived = "(received)";
const char* const kDefaultHarness = "claude";

SessionChange takeQueued() {
    SessionChange c;
    c.take = true;
    return c;
}

SessionChange infoChange(SessionInfo info) {
    SessionChange c;
    c.info = std::move(info);
    return c;
}

bool stepFits(SessionId id, const StoredSession* had, SessionId next, const std::vector<SessionChange>& changes,
              std::string* why) {
    auto refuse = [&](const std::string& what) {
        if (why != nullptr) {
            *why = what;
        }
        return false;
    };
    if (changes.empty()) {
        return id == 0 ? true : refuse("a step with no change names session " + std::to_string(id));
    }
    if (id == 0) {
        return refuse("a change names no session");
    }
    if (had == nullptr && id != next) {
        return refuse("session " + std::to_string(id) + " is not held, and the next made is " + std::to_string(next));
    }
    if (had == nullptr && !changes.front().info) {
        return refuse("session " + std::to_string(id) + " is not held, and the step does not make it");
    }
    // Every take must find a prompt in the queue at its point of the step.
    std::size_t queued = had == nullptr ? 0 : had->queue.size();
    for (const SessionChange& c : changes) {
        if (c.take && queued == 0) {
            return refuse("session " + std::to_string(id) + ": a take from a queue with nothing left in it");
        }
        if (c.take) {
            --queued;
        } else if (!c.driver && !c.entry && !c.info) {
            ++queued;
        }
    }
    return true;
}

namespace {

void applyTo(StoredSession& s, const SessionChange& c) {
    if (c.take) {
        s.queue.erase(s.queue.begin());
    } else if (c.info) {
        s.info = *c.info;
    } else if (c.driver) {
        s.driver = *c.driver;
    } else if (c.entry) {
        s.transcript.push_back(c.entry);
    } else {
        s.queue.push_back(c.prompt);
    }
}

} // namespace

const StoredSession* MemoryStore::find(SessionId id) const {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : &it->second;
}

std::vector<SessionId> MemoryStore::ids() const {
    std::vector<SessionId> out;
    for (const auto& [id, s] : sessions_) {
        out.push_back(id);
    }
    return out;
}

bool MemoryStore::commit(SessionId id, const std::vector<SessionChange>& changes, const std::string& client,
                         std::uint64_t num, std::string* why) {
    if (changes.empty() && client.empty()) {
        if (why != nullptr) {
            *why = "a step with no change and no client changes nothing";
        }
        return false;
    }
    if (!client.empty() && num != applied(client) + 1) {
        if (why != nullptr) {
            *why = "client " + client + ": action " + std::to_string(num) + " after " + std::to_string(applied(client));
        }
        return false;
    }
    if (!stepFits(id, find(id), next_, changes, why)) {
        return false;
    }
    if (!changes.empty()) {
        if (find(id) == nullptr) {
            ++next_;
        }
        StoredSession& s = sessions_[id];
        for (const SessionChange& c : changes) {
            applyTo(s, c);
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

Hub::Hub(SessionStore& store, Limits limits, Responder* responder)
    : store_(store), limits_(limits), responder_(responder) {
    for (SessionId id : store_.ids()) {
        refresh(id);
    }
}

namespace {

EntryRef entry(EntryKind kind, std::string text) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, std::move(text), true});
}

std::string kib(std::size_t bytes) {
    return std::to_string(bytes / 1024) + " KiB";
}

const char* verb(ActionKind kind) {
    switch (kind) {
        case ActionKind::Send: return "sent";
        case ActionKind::Queue: return "queued";
        default: return "applied";
    }
}

const char* settingName(ActionKind kind) {
    switch (kind) {
        case ActionKind::SetAccount: return "account";
        case ActionKind::SetModel: return "model";
        default: return "effort";
    }
}

bool printable(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c <= 0x7e; });
}

bool isSet(ActionKind kind) {
    return kind == ActionKind::SetAccount || kind == ActionKind::SetModel || kind == ActionKind::SetEffort;
}

} // namespace

// An action that changes no session: the client's counter moves in a step of its own, so it is never taken again.
bool Hub::count(const Action& action, std::string* why, const std::string& client) {
    if (client.empty()) {
        return true;
    }
    std::string failed;
    if (!store_.commit(0, {}, client, action.num, &failed)) {
        if (why != nullptr) {
            *why = "action " + std::to_string(action.num) + " was not counted: " + failed;
        }
        return false;
    }
    return true;
}

// Refused, changing no session, and counted.
bool Hub::counted(const Action& action, const std::string& refused, std::string* why, const std::string& client) {
    if (count(action, why, client) && why != nullptr) {
        *why = refused;
    }
    return false;
}

bool Hub::apply(const Action& action, std::string* why, const std::string& client) {
    if (action.kind == ActionKind::Create || action.kind == ActionKind::Ensure) {
        if (action.friendName.empty() || action.friendName.size() > limits_.friendName) {
            return counted(action, "a session's friend is named by 1 to " + std::to_string(limits_.friendName) +
                " bytes", why, client);
        }
        if (!action.text.empty()) {
            return counted(action, "a create or an ensure carries no text", why, client);
        }
        if (action.kind == ActionKind::Ensure) {
            for (SessionId id : store_.ids()) {
                if (store_.find(id)->info.friendName == action.friendName) {
                    // The friend has a session: nothing to make, and nothing refused.
                    return count(action, why, client);
                }
            }
        }
        SessionInfo info = responder_ != nullptr ? responder_->defaults(action.friendName)
                                                 : SessionInfo{action.friendName, kDefaultHarness, {}, {}, {}};
        info.friendName = action.friendName;
        if (action.kind == ActionKind::Create && action.session != 0) {
            const StoredSession* from = store_.find(action.session);
            if (from == nullptr) {
                return counted(action, "no session " + std::to_string(action.session) + " to copy", why, client);
            }
            info.account = from->info.account;
            info.model = from->info.model;
            info.effort = from->info.effort;
        }
        return make(action, info, why, client);
    }
    const StoredSession* session = store_.find(action.session);
    if (session == nullptr) {
        // Nothing runs in a session the server does not hold, and no session is made for it: refused, and counted.
        return counted(action, "no session " + std::to_string(action.session), why, client);
    }
    if (isSet(action.kind)) {
        return set(action, why, client);
    }
    std::string failed;
    auto commit = [&](const std::vector<SessionChange>& changes) {
        if (changes.empty() && client.empty()) {
            return true;    // nothing to change and no counter to move
        }
        if (store_.commit(changes.empty() ? 0 : action.session, changes, client, action.num, &failed)) {
            return true;
        }
        if (why != nullptr) {
            *why = "session " + std::to_string(action.session) + ": the action was not applied: " + failed;
        }
        return false;
    };
    std::string refused;
    if (action.kind == ActionKind::Stop) {
        if (!action.text.empty()) {
            return counted(action, "a stop that carried text was not applied: a stop carries none", why, client);
        }
    } else if (blankText(action.text)) {
        refused = std::string("An empty prompt was not ") + verb(action.kind) + ".";
    } else if (action.text.size() > limits_.text) {
        refused = "A prompt of " + std::to_string(action.text.size()) + " bytes was not " + verb(action.kind) +
            ": the limit is " + kib(limits_.text) + ".";
    } else if (action.kind == ActionKind::Queue) {
        const std::size_t bytes = std::accumulate(session->queue.begin(), session->queue.end(), std::size_t(0),
            [](std::size_t n, const std::string& q) { return n + q.size(); });
        if (session->queue.size() >= limits_.queue) {
            refused = "A prompt was not queued: the queue holds " + std::to_string(limits_.queue) +
                " prompts, its limit.";
        } else if (bytes + action.text.size() > limits_.queueBytes) {
            refused = "A prompt was not queued: the queue would hold more than " + kib(limits_.queueBytes) + ".";
        }
    }
    if (!refused.empty()) {
        if (!commit({SessionChange{entry(EntryKind::Notice, refused), {}}})) {
            return false;
        }
        refresh(action.session);
        if (why != nullptr) {
            *why = "session " + std::to_string(action.session) + ": " + refused;
        }
        return false;
    }
    std::vector<SessionChange> changes;
    if (action.kind == ActionKind::Send && responder_ != nullptr) {
        changes = responder_->send(action.session, action.text);
    } else if (action.kind == ActionKind::Send) {
        changes.push_back(SessionChange{entry(EntryKind::Person, action.text), {}});
        changes.push_back(SessionChange{entry(EntryKind::Friend, kReceived), {}});
    } else if (action.kind == ActionKind::Queue && responder_ != nullptr) {
        changes = responder_->queue(action.session, action.text);
    } else if (action.kind == ActionKind::Queue) {
        changes.push_back(SessionChange{nullptr, action.text});
    } else if (responder_ != nullptr) {
        changes = responder_->stop(action.session);
    }
    const bool committed = commit(changes);
    if (responder_ != nullptr) {
        responder_->sent(action.session, committed);
    }
    if (!committed) {
        return false;
    }
    refresh(action.session);
    return true;
}

// A new session, with the next id, in one step with the client's counter.
bool Hub::make(const Action& action, const SessionInfo& info, std::string* why, const std::string& client) {
    if (store_.size() >= limits_.sessions) {
        return counted(action, "no session made for " + action.friendName + ": the server holds " +
            std::to_string(limits_.sessions) + " sessions, its limit", why, client);
    }
    const SessionId id = store_.nextId();
    std::string failed;
    if (!store_.commit(id, {infoChange(info)}, client, action.num, &failed)) {
        if (why != nullptr) {
            *why = "no session made for " + action.friendName + ": " + failed;
        }
        return false;
    }
    refresh(id);
    return true;
}

// A session's account, model or effort, from its next turn on.
bool Hub::set(const Action& action, std::string* why, const std::string& client) {
    const StoredSession* session = store_.find(action.session);
    std::string refused;
    if (action.text.size() > limits_.setting || !printable(action.text)) {
        refused = std::string("The ") + settingName(action.kind) + " was not changed: a setting is at most " +
            std::to_string(limits_.setting) + " printable characters.";
    }
    SessionInfo info = session->info;
    if (action.kind == ActionKind::SetAccount) {
        info.account = action.text;
    } else if (action.kind == ActionKind::SetModel) {
        info.model = action.text;
    } else {
        info.effort = action.text;
    }
    std::vector<SessionChange> changes;
    if (!refused.empty()) {
        changes.push_back(SessionChange{entry(EntryKind::Notice, refused), {}});
    } else if (!(info == session->info)) {
        changes.push_back(infoChange(info));
    }
    std::string failed;
    if ((!changes.empty() || !client.empty()) &&
            !store_.commit(changes.empty() ? 0 : action.session, changes, client, action.num, &failed)) {
        if (why != nullptr) {
            *why = "session " + std::to_string(action.session) + ": the action was not applied: " + failed;
        }
        return false;
    }
    if (!refused.empty()) {
        refresh(action.session);
        if (why != nullptr) {
            *why = "session " + std::to_string(action.session) + ": " + refused;
        }
        return false;
    }
    if (changes.empty()) {
        return true;    // already so
    }
    refresh(action.session);
    if (responder_ != nullptr) {
        responder_->configured(action.session, info);
    }
    return true;
}

bool Hub::commit(SessionId id, const std::vector<SessionChange>& changes, std::string* why) {
    if (changes.empty()) {
        return true;
    }
    if (store_.find(id) == nullptr) {
        if (why != nullptr) {
            *why = "no session " + std::to_string(id);
        }
        return false;
    }
    if (!store_.commit(id, changes, std::string(), 0, why)) {
        return false;
    }
    refresh(id);
    return true;
}

bool Hub::setOpen(SessionId id, const std::string& text) {
    if (store_.find(id) == nullptr || text.size() > limits_.text) {
        return false;
    }
    auto it = open_.find(id);
    if (text.empty()) {
        if (it == open_.end()) {
            return false;
        }
        open_.erase(it);
    } else if (it != open_.end() && it->second->text == text) {
        return false;
    } else {
        open_[id] = std::make_shared<const SessionEntry>(SessionEntry{EntryKind::Friend, text, false});
    }
    refresh(id);
    return true;
}

// The session's new tail, in place of its old one; every other session is shared with the state before.
void Hub::refresh(SessionId id) {
    const StoredSession* stored = store_.find(id);
    auto open = open_.find(id);
    auto fresh = std::make_shared<const SessionView>(tailOf(id, stored->info, stored->transcript, stored->queue,
        limits_.tail, open == open_.end() ? nullptr : open->second));
    std::vector<SessionRef> sessions = state_.sessions();
    auto it = std::lower_bound(sessions.begin(), sessions.end(), id,
        [](const SessionRef& s, SessionId n) { return s->id < n; });
    if (it != sessions.end() && (*it)->id == id) {
        *it = fresh;
    } else {
        sessions.insert(it, fresh);
    }
    state_ = Sessions(std::move(sessions));
}

} // namespace nexus::sync
