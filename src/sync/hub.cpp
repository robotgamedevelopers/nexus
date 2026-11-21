#include "sync/hub.h"
#include "policy/session.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <numeric>

#include "sync/client.h"
#include "sync/codec.h"

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

Hub::Hub(SessionStore& store, Limits limits, Responder* responder, std::function<std::string()> bundlePath)
    : store_(store), limits_(limits), responder_(responder), bundlePath_(std::move(bundlePath)) {
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
    if (action.kind == ActionKind::SecretAnswer || action.kind == ActionKind::SecretCancel) {
        return secretAction(action, why, client);
    }
    if (action.kind == ActionKind::SignInClaim || action.kind == ActionKind::SignInOpened ||
            action.kind == ActionKind::SignInCallback) {
        return signInAction(action, why, client);
    }
    if (action.kind == ActionKind::SignOut) {
        return signOutAction(action, why, client);
    }
    if (action.kind == ActionKind::Unknown) {
        return counted(action, describe(action) + " is of a kind this server does not know: counted, not applied", why,
            client);
    }
    if (action.kind == ActionKind::ResolveLimit)
        return resolveLimit(action, why, client);
    if (action.kind >= ActionKind::AddAccount) {
        return accountAction(action, why, client);
    }
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
        const auto* f = accounts_ ? accounts_->friendNamed(action.friendName) : nullptr;
        const auto* from = action.kind == ActionKind::Create && action.session ? store_.find(action.session) : nullptr;
        if (action.kind == ActionKind::Create && action.session && !from)
            return counted(action, "no session " + std::to_string(action.session) + " to copy", why, client);
        const SessionInfo defaults = info;
        std::string error;
        const auto pool = subscriptionsFor(info.friendName);
        if (!policy::newSession(application_, bundlePath_(), defaults, from ? &from->info : nullptr, f,
                                bool(accounts_), pool.size() > 1, info, error))
            return counted(action, "New session application: " + error, why, client);
        if (info.autoAccount) {
            if (pool.size() < 2)
                return counted(action, "Automatic account selection needs two linked subscriptions.", why, client);
            if (!from || from->info.friendName != info.friendName || !from->info.autoAccount ||
                    !signedIn_.contains(info.account) || std::find(pool.begin(), pool.end(), info.account) == pool.end())
                info.account = firstAutoAccount(pool);
        }
        if (info.model.size() > limits_.setting || info.effort.size() > limits_.setting ||
                !printable(info.model) || !printable(info.effort) ||
                (accounts_ && !info.account.empty() && !accounts_->linked(action.friendName, info.account)) ||
                (accounts_ && !info.instance.empty() && (!f || !f->instance(info.instance))) ||
                (!accounts_ && info.account != defaults.account && (!from || info.account != from->info.account)) ||
                (!accounts_ && info.instance != defaults.instance &&
                    (!from || from->info.friendName != info.friendName || info.instance != from->info.instance)))
            return counted(action, "New session application: choose an available account and instance and "
                "printable model and effort within the setting limit", why, client);
        if (accounts_)
            info.harness = harnessOf(info.account, info.harness);
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
    return applyMessage(action, *session, why, client);
}

bool Hub::applyMessage(const Action& action, const StoredSession& session, std::string* why, const std::string& client) {
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
    std::vector<SessionChange> notices;
    policy::MessagePlan plan;
    std::string refused;
    ActionKind kind = action.kind;
    const std::string* text = &action.text;
    if (kind != ActionKind::Stop && text->size() <= limits_.text) {
        const policy::MessageState state{*text, session.info.friendName, session.info.model, session.queue.size(),
            session.driver.openTurn != 0};
        if (!policy::messagePlan(application_, bundlePath_(), false, kind == ActionKind::Queue ? "queue" : "send", state, plan, refused)) {
            refused = "Message application: " + refused;
        } else {
            for (const auto& notice : plan.notices)
                notices.emplace_back(entry(EntryKind::Notice, notice), std::string());
            if (plan.text)
                text = &*plan.text;
            if (plan.delivery == policy::Delivery::Native) {
                auto service = services_.find(plan.service);
                if (service == services_.end())
                    refused = "No native service registered as " + plan.service + ".";
                else if (text->size() > limits_.text)
                    refused = "Native service input exceeds the message limit.";
                else {
                    try {
                        const auto result = service->second(*text);
                        if (result.size() > limits_.text)
                            refused = "Native service output exceeds the message limit.";
                        else
                            notices.emplace_back(entry(EntryKind::Notice, result), std::string());
                    } catch (const std::exception&) {
                        refused = "Native service " + plan.service + " failed to handle the request.";
                    }
                }
            }
            if (refused.empty() && (plan.delivery == policy::Delivery::None || plan.delivery == policy::Delivery::Native)) {
                if (!commit(notices))
                    return false;
                refresh(action.session);
                return true;
            }
            kind = plan.delivery == policy::Delivery::Send ? ActionKind::Send : ActionKind::Queue;
        }
    }
    if (refused.empty()) {
        if (kind == ActionKind::Stop) {
            if (!text->empty()) {
                return counted(action, "a stop that carried text was not applied: a stop carries none", why, client);
            }
        } else if (blankText(*text)) {
            refused = std::string("An empty prompt was not ") + verb(kind) + ".";
        } else if (text->size() > limits_.text) {
            refused = "A prompt of " + std::to_string(text->size()) + " bytes was not " + verb(kind) +
                ": the limit is " + kib(limits_.text) + ".";
        } else if (kind == ActionKind::Queue) {
            const std::size_t bytes = std::accumulate(session.queue.begin(), session.queue.end(), std::size_t(0),
                [](std::size_t n, const std::string& q) { return n + q.size(); });
            if (session.queue.size() >= limits_.queue) {
                refused = "A prompt was not queued: the queue holds " + std::to_string(limits_.queue) +
                    " prompts, its limit.";
            } else if (bytes + text->size() > limits_.queueBytes) {
                refused = "A prompt was not queued: the queue would hold more than " + kib(limits_.queueBytes) + ".";
            }
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
    if (kind == ActionKind::Send && responder_ != nullptr) {
        changes = responder_->send(action.session, *text);
    } else if (kind == ActionKind::Send) {
        changes.push_back(SessionChange{entry(EntryKind::Person, *text), {}});
        changes.push_back(SessionChange{entry(EntryKind::Friend, kReceived), {}});
    } else if (kind == ActionKind::Queue && responder_ != nullptr) {
        changes = responder_->queue(action.session, *text);
    } else if (kind == ActionKind::Queue) {
        changes.push_back(SessionChange{nullptr, *text});
    } else if (responder_ != nullptr) {
        changes = responder_->stop(action.session);
    }
    changes.insert(changes.begin(), notices.begin(), notices.end());
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
    if (responder_ != nullptr) {
        responder_->made(id);
    }
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
    std::string account = action.text;
    const bool chooseAuto = action.kind == ActionKind::SetAccount && account == kAutoAccountTarget;
    if (refused.empty() && chooseAuto) {
        const auto pool = subscriptionsFor(info.friendName);
        if (pool.size() < 2)
            refused = "Link at least two subscription accounts in friends.json to use auto.";
        else
            account = firstAutoAccount(pool);
    } else if (refused.empty() && action.kind == ActionKind::SetAccount && account.starts_with('+')) {
        const auto* who = accounts_ ? accounts_->friendNamed(info.friendName) : nullptr;
        if (!who || who->accounts.empty()) {
            refused = "Link an account to " + info.friendName + " in friends.json to switch accounts.";
        } else {
            account = policy::accountCommand(application_, bundlePath_(), info.account, who->accounts,
                                             account.substr(1), refused);
        }
    }
    if (refused.empty() && action.kind == ActionKind::SetAccount && accounts_ && !account.empty() &&
            !accounts_->linked(info.friendName, account)) {
        refused = "The account was not changed: " + account + " is not one of " + info.friendName +
            "'s accounts" + (accounts_->account(account) == nullptr ? " (no account " + account +
            " is set up in accounts.json)" : "") + ". Link it to " + info.friendName + " first: add it to " +
            info.friendName + "'s \"accounts\" in friends.json and run `nexus-server config reload` "
            "(docs/accounts.md).";
    }
    if (action.kind == ActionKind::SetAccount) {
        info.account = account;
        info.autoAccount = chooseAuto;
        if (accounts_) {
            info.harness = harnessOf(info.account, info.harness);
        }
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
    if (action.kind == ActionKind::SetAccount && signsIn(info.account)) {
        needSignIn(info.account);
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

std::vector<std::string> Hub::subscriptionsFor(const std::string& friendName) const {
    std::vector<std::string> pool;
    const auto* who = accounts_ ? accounts_->friendNamed(friendName) : nullptr;
    if (who) {
        for (const auto& name : who->accounts) {
            const auto* account = accounts_->account(name);
            if (account && account->billing == accounts::Billing::Subscription)
                pool.push_back(name);
        }
    }
    return pool;
}

std::string Hub::firstAutoAccount(const std::vector<std::string>& pool) const {
    for (const auto& name : pool)
        if (signedIn_.contains(name)) return name;
    // With no usable login, the concrete first choice can show its sign-in card.
    return pool.empty() ? std::string{} : pool.front();
}

std::string Hub::harnessOf(const std::string& account, const std::string& otherwise) const {
    const accounts::Account* a = accounts_ && !account.empty() ? accounts_->account(account) : nullptr;
    return a == nullptr ? otherwise : a->provider;
}

// A session's new settings, from the server rather than the person, with the notice that tells it why: one step.
bool Hub::moveTo(SessionId id, const SessionInfo& info, const std::string& notice) {
    std::string failed;
    if (!store_.commit(id, {infoChange(info), SessionChange{entry(EntryKind::Notice, notice), {}}}, std::string(), 0,
                       &failed)) {
        return false;
    }
    refresh(id);
    if (responder_ != nullptr) {
        responder_->configured(id, info);
    }
    return true;
}

// Every session on an account no longer linked to its friend moves off it; every session whose account's provider
// changed takes its harness. Each is told. A session whose friend is not in the registry is left as it is until its
// friend is back; one whose friend this change took out is told, once, and its responder stops what runs in it. False
// when the store did not take a step.
bool Hub::settle(std::vector<std::string>* report, const std::string& cause, const accounts::Registry* before) {
    for (SessionId id : store_.ids()) {
        const std::string friendName = store_.find(id)->info.friendName;
        const bool was = before != nullptr && before->friendNamed(friendName) != nullptr;
        if (accounts_->friendNamed(friendName) == nullptr) {
            if (was && !removed(id, friendName, cause, report)) {
                return false;
            }
            continue;
        }
        if (!settleOne(id, report, cause, before)) {
            return false;
        }
        if (before != nullptr && !was && responder_ != nullptr) {
            responder_->present(id, true);    // its friend is back: what waits goes on
        }
    }
    return true;
}

// A session whose friend a change took out of friends.json: one notice, then its responder stops what runs in it. Its
// account, instance, transcript and queue are kept as they are.
bool Hub::removed(SessionId id, const std::string& friendName, const std::string& cause,
                  std::vector<std::string>* report) {
    const std::string notice = friendName + " was taken out of friends.json (" + cause + "), so nexus-server stopped "
        "any work running in this session and ended its driver. This session, its transcript and its queued prompts "
        "are kept, and the queued prompts go on if " + friendName + " is added to friends.json again.";
    std::string failed;
    if (!store_.commit(id, {SessionChange{entry(EntryKind::Notice, notice), {}}}, std::string(), 0, &failed)) {
        return false;
    }
    refresh(id);
    if (report != nullptr) {
        report->push_back("session " + std::to_string(id) + " (" + friendName + "): its friend was removed, so its work "
            "was stopped; the session is kept");
    }
    if (responder_ != nullptr) {
        responder_->present(id, false);
    }
    return true;
}

// One session whose friend is in the registry, settled against it.
bool Hub::settleOne(SessionId id, std::vector<std::string>* report, const std::string& cause,
                    const accounts::Registry* before) {
    auto where = [](const accounts::Instance& in) { return "on " + in.machine + " in " + in.dir; };
    SessionInfo info = store_.find(id)->info;
    // A session belongs to one of its friend's instances, or to none when the friend has none. An instance the
    // friend no longer has: the session moves to the friend's default instance (its first), or none, told. No
    // instance while the friend has some: the default. Its instance moved in place (the same name, another
    // machine or directory): told, and its driver follows (Drivers::configured).
    const accounts::Friend* fr = accounts_->friendNamed(info.friendName);   // in the registry: settle() saw to it
    const accounts::Instance* in = info.instance.empty() ? nullptr : fr->instance(info.instance);
    const accounts::Friend* frBefore = before == nullptr ? nullptr : before->friendNamed(info.friendName);
    const accounts::Instance* inBefore = frBefore == nullptr || in == nullptr ? nullptr :
        frBefore->instance(info.instance);
    std::string instanceNotice;
    if (info.instance.empty() && !fr->instances.empty()) {
        const accounts::Instance& first = fr->instances.front();
        info.instance = first.name;
        instanceNotice = "This session now belongs to " + info.friendName + "'s default instance, " + first.name +
            " (" + where(first) + "): a session belongs to one of its friend's instances (" + cause + ").";
        if (report != nullptr) {
            report->push_back("session " + std::to_string(id) + " (" + info.friendName + "): instance none -> " +
                info.instance);
        }
    } else if (inBefore != nullptr && (inBefore->machine != in->machine || inBefore->dir != in->dir)) {
        instanceNotice = info.friendName + "'s instance " + in->name + " moved (" + cause + "): it was " +
            where(*inBefore) + " and is now " + where(*in) + ". This session's driver restarts there once no "
            "turn is open: in the same directory it resumes the conversation; in another it starts afresh, given "
            "a summary of the conversation so far.";
        if (report != nullptr) {
            report->push_back("session " + std::to_string(id) + " (" + info.friendName + "): instance " +
                in->name + " moved, " + where(*inBefore) + " -> " + where(*in));
        }
    } else if (!info.instance.empty() && in == nullptr) {
        const std::string wasInstance = info.instance;
        info.instance = fr->instances.empty() ? std::string() : fr->instances.front().name;
        instanceNotice = "The instance " + wasInstance + " is no longer one of " + info.friendName + "'s (" +
            cause + "), so this session now belongs to " + (info.instance.empty() ? std::string("none of its "
            "instances") : "its instance " + info.instance) + ". Add an instance to " + info.friendName +
            " in friends.json to choose another.";
        if (report != nullptr) {
            report->push_back("session " + std::to_string(id) + " (" + info.friendName + "): instance " +
                wasInstance + " -> " + (info.instance.empty() ? std::string("none") : info.instance));
        }
    }
    // Its friend's "limit" changed (a reload): told. Tightened, an open turn is stopped now and the driver restarts
    // limited (Supervisor::setLimited); loosened, the driver restarts unlimited once no turn is open. A friend put back
    // in friends.json (frBefore null) takes its record's limit through Drivers::configured like any other start.
    if (frBefore != nullptr && fr->limit != frBefore->limit) {
        instanceNotice += std::string(instanceNotice.empty() ? "" : " ") + info.friendName + " is " +
            (fr->limit ? "now limited to its directory" : "no longer limited to its directory") + " (" + cause +
            "): this session's driver restarts " + (fr->limit ? "limited; a turn already open is stopped now." :
            "unlimited once no turn is open; a turn already open finishes as it started.");
        if (report != nullptr) {
            report->push_back("session " + std::to_string(id) + " (" + info.friendName + "): limit " +
                (frBefore->limit ? "true" : "false") + " -> " + (fr->limit ? "true" : "false"));
        }
    }
    // The directories a limited friend is kept out of changed (friends.json "limit_keeps_out"): told in each limited
    // session, whose driver restarts keeping out the new list (Supervisor::setKeepsOut): a turn open is stopped now
    // when more are kept out, and finishes as it started when fewer are.
    if (before != nullptr && before->keepsOut != accounts_->keepsOut &&
        accounts::limitedFor(&*accounts_, info.friendName, info.account)) {
        std::string dirs;
        for (const std::string& d : accounts_->keepsOut) {
            dirs += (dirs.empty() ? "" : ", ") + d;
        }
        instanceNotice += std::string(instanceNotice.empty() ? "" : " ") + "The directories a limited friend is "
            "kept out of, on top of its own, are now " + (dirs.empty() ? std::string("none") : dirs) + " (" + cause +
            "): this session's driver restarts keeping them out; a turn already open is stopped now if more are "
            "kept out, and otherwise finishes as it started.";
    }
    const std::string was = info.account;
    std::string notice;
    if (info.autoAccount) {
        const auto pool = subscriptionsFor(info.friendName);
        if (pool.size() < 2) {
            info.autoAccount = false;
            notice = "Automatic account selection ended because fewer than two subscription accounts remain linked to " +
                info.friendName + " (" + cause + ").";
        }
        if (std::find(pool.begin(), pool.end(), was) == pool.end()) {
            info.account = firstAutoAccount(pool);
            info.harness = harnessOf(info.account, info.harness);
            if (!notice.empty()) notice += " ";
            notice += info.account.empty() ? "No subscription account is selected." :
                "The selected subscription account is now " + info.account + ".";
        }
    } else if (!was.empty() && !accounts_->linked(info.friendName, was)) {
        info.account = accounts::landing(*accounts_, signedIn_, info.friendName);
        info.harness = harnessOf(info.account, info.harness);
        notice = "The account " + was + " is no longer one of " + info.friendName + "'s accounts (" + cause +
            "), so this session now runs on " + (info.account.empty() ? std::string("no account (the harness's own "
            "login)") : "the account " + info.account) + ". Link an account to " + info.friendName + " in "
            "friends.json to choose another.";
    }
    if (info.account == was && info.harness != harnessOf(was, info.harness)) {
        info.harness = harnessOf(was, info.harness);
        if (!notice.empty()) notice += " ";
        notice += "The account " + was + "'s provider is now " + info.harness + " (" + cause + "), so this session "
            "now runs " + info.harness + ".";
    } else if (const auto* row = accounts_->account(was); before && row && row->provider == "claude" &&
               before->account(was) && before->account(was)->email != row->email) {
        notice = "The configured Claude email for " + was + " changed. This session will use the updated login.";
    } else if (notice.empty() && instanceNotice.empty()) {
        return true;
    }
    if (!moveTo(id, info, instanceNotice.empty() ? notice : notice.empty() ? instanceNotice :
                instanceNotice + " " + notice)) {
        return false;
    }
    if (notice.empty()) {
        return true;    // its instance alone moved: said above
    }
    if (report != nullptr) {
        report->push_back("session " + std::to_string(id) + " (" + info.friendName + "): account " + was + " -> " +
            (info.account.empty() ? std::string("none") : info.account) + ", harness " + info.harness);
    }
    return true;
}

namespace {

using AccountIndex = std::map<std::string_view, const accounts::Account*>;

AccountIndex indexAccounts(const accounts::Registry& registry) {
    AccountIndex index;
    for (const auto& account : registry.accounts) {
        index.emplace(account.name, &account);
    }
    return index;
}

std::vector<ServerFriend> windowRoster(const accounts::Registry& registry, const AccountIndex& index,
                                      const std::set<std::string>* signedIn = nullptr, bool canSignIn = false) {
    std::vector<ServerFriend> friends;
    friends.reserve(registry.friends.size());
    for (const auto& f : registry.friends) {
        ServerFriend& row = friends.emplace_back(ServerFriend{f.name});
        row.accounts.reserve(f.accounts.size());
        std::size_t subscriptions = 0;
        for (const auto& name : f.accounts) {
            if (const auto a = index.find(name); a != index.end()) {
                // Measure admission with every optional sign-in bit set.
                const bool needs = !signedIn || (canSignIn && hasSignIn(a->second->provider) &&
                                                  !signedIn->contains(name));
                row.accounts.push_back({name, a->second->label, needs, a->second->email});
                if (a->second->billing == accounts::Billing::Subscription) ++subscriptions;
            }
        }
        row.canAutoAccount = subscriptions > 1;
    }
    return friends;
}

} // namespace

std::string accountRosterProblem(const accounts::Registry& registry) {
    const auto index = indexAccounts(registry);
    const char* problem = "friends.json: the linked account choices exceed the window roster budget "
                          "(3 MiB on the wire or 12 MiB decoded); shorten repeated account labels or emails in accounts.json "
                          "or remove links from friends.json";
    std::size_t textBytes = 0;
    std::size_t rows = 0;
    for (const auto& f : registry.friends) {
        textBytes += f.name.size();
        rows++;
        for (const auto& name : f.accounts) {
            if (const auto account = index.find(name); account != index.end()) {
                // Each string is written inline for every link. Check before expanding shared
                // account metadata into thousands of distinct entries in memory.
                textBytes += name.size() + account->second->label.size() + account->second->email.size();
                rows++;
                if (textBytes > codec::kRosterWireBytes) {
                    return problem;
                }
            }
        }
    }
    // Small rosters are far below both generated sizes even with per-field framing, aligned
    // string storage, and replacement of invalid UTF-8. Measure all larger ones with the real codec.
    if (rows <= 1024 && textBytes <= 64 * 1024) {
        return {};
    }
    return codec::rosterFits(windowRoster(registry, index)) ? "" : problem;
}

std::vector<std::string> Hub::useAccounts(accounts::Registry registry, AccountsKeeper* keeper) {
    if (auto problem = accountRosterProblem(registry); !problem.empty()) {
        return {std::move(problem)};
    }
    take(std::move(registry));
    keeper_ = keeper;
    std::vector<std::string> report;
    settle(&report, "as the server started");
    return report;
}

bool Hub::applyAccounts(const accounts::Registry& next, const std::string& message, std::vector<std::string>* report) {
    std::vector<std::string> lines = accounts::problems(next);
    if (lines.empty()) {
        if (auto problem = accountRosterProblem(next); !problem.empty()) {
            lines.push_back(std::move(problem));
        }
    }
    std::string failed;
    if (lines.empty() && keeper_ != nullptr && !keeper_->keep(next, message, &failed)) {
        lines.push_back(failed);
    }
    if (!lines.empty()) {
        if (report != nullptr) {
            *report = std::move(lines);
        }
        return false;
    }
    // With none held before (the committed files did not read at the start), no friend was known to be in or out: the
    // sessions are settled as at a start, and then each is told, from `next` alone, whether its friend is in it. It is
    // never diffed against an empty registry, which would read every friend as put back (models/Supervisor.tla Learn).
    const bool unknown = !accounts_;
    const accounts::Registry before = accounts_ ? *accounts_ : accounts::Registry{};
    lines = accounts::changes(before, next);
    take(next);
    const std::string cause = "the accounts and friends were changed: " + message;
    if (!settle(&lines, cause, unknown ? nullptr : &before)) {
        lines.push_back("the store failed while the sessions were moved, so the server stops; at its next start it "
            "moves them, against what was committed");
    } else if (unknown && responder_ != nullptr) {
        for (SessionId id : store_.ids()) {
            responder_->present(id, accounts_->friendNamed(store_.find(id)->info.friendName) != nullptr);
        }
    }
    if (report != nullptr) {
        *report = std::move(lines);
    }
    return true;    // committed: applied, whatever the store did after
}

// The registry becomes `next`; an account it no longer holds loses its sign-in, and one it did not hold before is
// probed (probeSignInWith).
static bool sameLogin(const accounts::Account* previous, const accounts::Account& current) {
    return previous && previous->provider == current.provider &&
        (current.provider != "claude" || previous->email == current.email);
}

void Hub::take(accounts::Registry next) {
    std::optional<accounts::Registry> was = std::move(accounts_);
    accounts_ = std::move(next);
    for (auto it = signedIn_.begin(); it != signedIn_.end();) {
        const auto* current = accounts_->account(*it);
        const auto* previous = was ? was->account(*it) : nullptr;
        it = !current || (previous && !sameLogin(previous, *current))
            ? signedIn_.erase(it) : std::next(it);
    }
    for (const accounts::Account& a : accounts_->accounts) {
        const accounts::Account* before = was ? was->account(a.name) : nullptr;
        if (!sameLogin(before, a)) {
            if (before && a.requiresVerifiedEmail()) secretEffects(desk_.retireLogin(a.name, loginFile(a.provider)));
            if (probe_ && probe_(a)) signedIn_.insert(a.name);
        }
    }
    stateFriends();
    // A flow for an account removed, or now of another provider, ends with it.
    for (const SignInOffer& o : signIns_.offers()) {
        const accounts::Account* a = accounts_->account(o.account);
        if (!a || (was && !sameLogin(was->account(o.account), *a))) {
            signInEffects(signIns_.signedIn(o.account));
        }
    }
    needAll();
}

void Hub::reportSignedIn(const std::string& account, bool signedIn) {
    if (accounts_ && accounts_->account(account) == nullptr) {
        return;
    }
    const auto* accountRow = accounts_ ? accounts_->account(account) : nullptr;
    if (signedIn && accountRow && accountRow->requiresVerifiedEmail()) {
        signedIn = probe_ && probe_(*accountRow);
    }
    if (signedIn) {
        if (signedIn_.insert(account).second && accounts_) stateFriends();
        // Signed in otherwise (a paste, its driver): a flow for it ends, its process ended.
        if (signIns_.live(account)) {
            signInEffects(signIns_.signedIn(account));
        }
        return;
    }
    if (signedIn_.erase(account) && accounts_) stateFriends();
    // An account whose provider signs in in the app: a flow, one browser sign-in, unless one is live or a login for it
    // is held already.
    if (signsIn(account)) {
        needSignIn(account);
        return;
    }
    // An account with no login it can use: nexus itself asks for it in the window (docs/secrets.md), one paste, unless
    // a request for it is open, or its value held, already. With no window that can take one, the ask is refused at
    // once and logged.
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (secretKeeper_ == nullptr || a == nullptr || loginFile(a->provider).empty() || desk_.busy(account)) {
        return;
    }
    const LoginAsk la = loginAsk_ ? loginAsk_(*a) : LoginAsk{true, account + " has no login it can use: paste its " +
                                                                   loginFile(a->provider) + " to sign it in"};
    if (la.ask) {
        askSecret(SecretAsker{}, account, loginFile(a->provider), la.text);
    } else {
        tellAccount(account, la.text);
    }
}

void Hub::needsSignIn(const std::string& account, const std::string& why) {
    if (accounts_ && accounts_->account(account) == nullptr) {
        return;
    }
    if (signedIn_.erase(account) && accounts_) stateFriends();
    if (signsIn(account)) {
        needSignIn(account);
        return;
    }
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (secretKeeper_ != nullptr && a != nullptr && !loginFile(a->provider).empty() && !desk_.busy(account)) {
        askSecret(SecretAsker{}, account, loginFile(a->provider), why);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// In-app sign-in

void Hub::signInWith(SignInHooks hooks, std::string host) {
    signInHooks_ = std::move(hooks);
    signIns_.setHost(std::move(host));
    if (accounts_) stateFriends();
    needAll();
}

bool Hub::signsIn(const std::string& account) const {
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    return signInHooks_.has_value() && secretKeeper_ != nullptr && a != nullptr && hasSignIn(a->provider);
}

// Every account that signs in in the app and has no login: a flow each (none for one live or held already).
void Hub::needAll() {
    if (!signInHooks_ || !accounts_) {
        return;
    }
    for (const accounts::Account& a : accounts_->accounts) {
        if (!a.signInOnUse && signedIn_.count(a.name) == 0 && signsIn(a.name)) {
            needSignIn(a.name);
        }
    }
}

bool Hub::pasteInstead() const {
    return signInHooks_ && signInHooks_->takers && takers_ && takers_() && !signInHooks_->takers();
}

void Hub::askPasteLogin(const std::string& account) {
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (a == nullptr || signedIn_.count(account) != 0 || loginFile(a->provider).empty() || desk_.busy(account)) {
        return;
    }
    // At most 64 + 95 bytes, inside kSecretWhyBytes.
    askSecret(SecretAsker{}, account, loginFile(a->provider), account + " has no login, and this nexus window is too "
              "old to sign in in the browser: paste its " + loginFile(a->provider));
}

void Hub::needSignIn(const std::string& account) {
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (a == nullptr || signedIn_.count(account) != 0 || signIns_.live(account) || desk_.busy(account)) {
        return;
    }
    if (pasteInstead() && !a->requiresVerifiedEmail()) {
        askPasteLogin(account);
        return;
    }
    // The id first, durable, so that it is never minted again, across restarts.
    const std::uint64_t id = store_.applied(kSignInIdsClient) + 1;
    std::string failed;
    if (!store_.commit(0, {}, kSignInIdsClient, id, &failed)) {
        if (secretEvents_ != nullptr) {
            secretEvents_->signInLogged(account + ": a sign-in was not started: its id was not kept: " + failed);
        }
        return;
    }
    signInEffects(signIns_.need(id, account, a->provider));
}

bool Hub::requestSignIn(const std::string& account, std::string* said) {
    if (!accounts::isName(account)) {
        *said = "an account is a name (letters, digits, '-', '_' and '.', at most 64)";
        return false;
    }
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (a == nullptr) {
        *said = "no account " + account + " is set up in accounts.json (docs/accounts.md)";
        return false;
    }
    if (!hasSignIn(a->provider)) {
        *said = account + "'s provider, " + a->provider + ", does not sign in in nexus's window";
        return false;
    }
    if (!signsIn(account)) {
        *said = "this server does not sign accounts in in the window";
        return false;
    }
    if (signedIn_.count(account) != 0) {
        *said = "signed in";
        return true;
    }
    if (a->requiresVerifiedEmail() && pasteInstead()) {
        *said = "Open an updated Nexus window to verify " + a->email + " in the browser.";
        return false;
    }
    const bool before = desk_.busy(account);
    needSignIn(account);
    *said = before ? "needs sign-in: its login is being stored, or asked for by a paste" :
        signIns_.live(account) ? "needs sign-in: the person signs it in in the browser from nexus's window" :
        desk_.busy(account) ? "needs sign-in: the person pastes its login in nexus's window, which is too old to "
                              "sign in in the browser" :
        "needs sign-in: it could not be asked for now (server.log says why)";
    return true;
}

void Hub::signInUrl(const std::string& account, std::uint64_t id, const SecretValue& url) {
    signInEffects(signIns_.url(account, id, url));
}

void Hub::signInPrinted(const std::string& account, const SecretValue& token) {
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (!a || a->provider != "claude" || a->requiresVerifiedEmail() || !secretKeeper_) {
        return;
    }
    const auto flow = signIns_.flowOf(account);
    const bool running = flow && signInRunning(flow->second);
    // The re-check: a subscription's login, not an API key (nor anything else the process printed).
    if (!isSubscriptionToken(token)) {
        if (!running) {
            if (secretEvents_ != nullptr) {
                secretEvents_->signInLogged(account + ": a sign-in's process printed, after its sign-in ended, a "
                                            "login that is not a subscription's: dropped");
            }
            return;
        }
        SignInEffects e = signIns_.exited(account, flow->first, "it printed a login that is not a subscription's");
        signInEffects(e);
        if (signInHooks_ && signInHooks_->stop) {
            signInHooks_->stop(account);
        }
        return;
    }
    // In time, the flow is captured; late (its deadline passed while the browser was sent back, or in the grace after
    // its process was told to end), the login is stored all the same and the account signed in: the person signed in,
    // and asking again would be the bug (models/SignIn.tla Print, EveryPrintTaken).
    SignInEffects e;
    signIns_.printed(account, &e);
    signInEffects(e);
    // Straight into the one-chance store, as a request of nexus's own (never in any window's state): stored when
    // durable, else held and written again; never dropped.
    const std::uint64_t id = store_.applied(kSecretIdsClient) + 1;
    std::string failed;
    if (!store_.commit(0, {}, kSecretIdsClient, id, &failed)) {
        // The store has failed (and the server stops): the login is written all the same, never dropped. Its id names
        // it in this server's memory alone.
        if (secretEvents_ != nullptr) {
            secretEvents_->signInLogged(account + ": the login's request id was not kept (" + failed +
                                        "); it is written all the same");
        }
    }
    secretEffects(desk_.capture(id, account, loginFile(a->provider), token, *secretKeeper_, clock_ ? clock_() : 0));
    // Refused as not of the login's shape: the account needs a fresh sign-in.
    if (signedIn_.count(account) == 0 && !desk_.busy(account)) {
        needSignIn(account);
    }
}

void Hub::signInEnrolled(const std::string& account, const std::string& provider, std::uint64_t id) {
    const auto flow = signIns_.flowOf(account);
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (!flow || flow->first != id || !signInRunning(flow->second) || a == nullptr || a->provider != provider) {
        return;
    }
    if (!probe_ || !probe_(*a)) {
        signInExited(account, id, "the provider did not confirm this account's enrollment");
        return;
    }
    SignInEffects effects;
    signIns_.printed(account, &effects);
    signInEffects(effects);
    reportSignedIn(account, true);
    if (responder_ != nullptr) {
        responder_->signedIn(account);
    }
}

void Hub::signInExited(const std::string& account, std::uint64_t id, const std::string& why) {
    auto effects = signIns_.exited(account, id, why);
    const auto* a = accounts_ ? accounts_->account(account) : nullptr;
    if (a && a->requiresVerifiedEmail() && !effects.notes.empty() && !why.empty()) {
        effects.notes.front().text = "Could not sign in to " + a->email + ". " + why;
    }
    signInEffects(effects);
}

void Hub::signInWindowsChanged() {
    if (!pasteInstead()) {
        return;
    }
    SignInEffects out;
    std::set<std::string> browserOnly;
    if (accounts_) {
        for (const auto& a : accounts_->accounts) {
            if (a.requiresVerifiedEmail()) browserOnly.insert(a.name);
        }
    }
    const std::vector<std::string> accounts = signIns_.fallBack(&out, browserOnly);
    signInEffects(out);
    for (const std::string& account : accounts) {
        askPasteLogin(account);
    }
}

void Hub::signInWindowGone(const std::string& opener) {
    signInEffects(signIns_.windowGone(opener));
}

void Hub::expireSignIns() {
    if (clock_) {
        signInEffects(signIns_.expire(clock_()));
    }
}

void Hub::dropSignIns() {
    signInEffects(signIns_.drop());
}

std::vector<SignInOffer> Hub::publicOffers() const {
    std::vector<SignInOffer> offers = signIns_.offers();
    for (SignInOffer& o : offers) {
        o.url.clear();
        if (!o.opener.empty()) {
            o.opener = kSignInHeldBy;
        }
    }
    return offers;
}

Sessions Hub::stateFor(const std::string& opener, const std::string& client) const {
    Sessions s = state_;
    if (!client.empty()) s.setNextAction(store_.applied(client) + 1);
    std::vector<SignInOffer> offers = signIns_.offers();
    if (opener.empty() || offers.empty()) {
        return s;
    }
    for (SignInOffer& o : offers) {
        if (o.opener != opener) {
            o.url.clear();
            if (!o.opener.empty()) {
                o.opener = kSignInHeldBy;
            }
        }
    }
    s.setOffers(std::move(offers));
    return s;
}

bool Hub::signOutAction(const Action& action, std::string* why, const std::string& client) {
    const auto* session = store_.find(action.session);
    if (!session)
        return counted(action, "The selected session no longer exists. Select a friend and retry.", why, client);
    // Count before removing a credential, so an interrupted command cannot remove a replacement on replay.
    if (!count(action, why, client)) return false;
    std::string error;
    const auto& name = action.account;
    const auto* account = accounts_ ? accounts_->account(name) : nullptr;
    const auto flow = signIns_.flowOf(name);
    if (session->info.autoAccount) {
        error = "Select a named account before signing out.";
    } else if (!account || session->info.account != name || !accounts_->linked(session->info.friendName, name)) {
        error = "The account selection changed. Select the account to sign out and try again.";
    } else if (account->provider != "claude" || !signOut_) {
        error = "This server supports sign-out only for a configured Claude login.";
    } else if (desk_.busy(name) || (flow && flow->second != SignInState::Needed)) {
        error = "Finish or cancel the open sign-in for this account, then retry /logout.";
    } else {
        error = signOut_(*account);
    }
    if (error.empty()) {
        signedIn_.erase(name);
        stateFriends();
    }
    const auto message = error.empty() ? "Signed out of " + name + " in Nexus." : error;
    std::string failed;
    if (!store_.commit(action.session, {SessionChange{entry(EntryKind::Notice, message), {}}}, {}, 0, &failed)) {
        if (why) *why = failed;
        return false;
    }
    refresh(action.session);
    if (why) *why = error;
    return error.empty();
}

bool Hub::signInAction(const Action& action, std::string* why, const std::string& client) {
    if (!count(action, why, client)) {
        return false;
    }
    if (!signInHooks_) {
        if (why != nullptr) {
            *why = describe(action) + " dropped: this server signs nothing in in the window";
        }
        return false;
    }
    switch (action.kind) {
        case ActionKind::SignInClaim: {
            auto id = action.request;
            if (!action.account.empty() || action.session != 0) {
                // A card or Enter names the exact session/account the person chose. Recheck it
                // after any pending account changes and before allocating or claiming a flow.
                const auto* session = store_.find(action.session);
                if (!isClaimToken(action.text) || !session || action.account.empty() ||
                    session->info.account != action.account || !signsIn(action.account) ||
                    !accounts_->linked(session->info.friendName, action.account) || signedIn_.contains(action.account)) {
                    if (why) *why = "The selected session or its sign-in changed. Use the current account's sign-in.";
                    return false;
                }
                if (!id) needSignIn(action.account);
                const auto flow = signIns_.flowOf(action.account);
                if (!flow || (id && flow->first != id)) {
                    if (why) *why = "The sign-in offer changed or could not be created. Retry the current account.";
                    return false;
                }
                id = flow->first;
            }
            signInEffects(signIns_.claim(id, action.text, clock_ ? clock_() : 0));
            break;
        }
        case ActionKind::SignInOpened:
            signInEffects(signIns_.opened(action.request, action.text));
            break;
        default:
            signInEffects(signIns_.callback(action.request, action.text, action.secret));
            break;
    }
    return true;
}

void Hub::noteAccount(const std::string& account, const std::string& text) {
    for (SessionId id : store_.ids()) {
        const StoredSession* s = store_.find(id);
        if (s == nullptr || s->info.account != account) {
            continue;
        }
        std::string failed;
        if (store_.commit(id, {SessionChange{entry(EntryKind::Notice, text), {}}}, std::string(), 0, &failed)) {
            refresh(id);
        }
    }
}

void Hub::signInEffects(const SignInEffects& effects) {
    for (const SignInEffects::Note& n : effects.notes) {
        noteAccount(n.account, n.text);
    }
    if (secretEvents_ != nullptr) {
        for (const std::string& l : effects.log) {
            secretEvents_->signInLogged(l);
        }
    }
    if (effects.changed) {
        state_.setOffers(publicOffers());
    }
    if (signInHooks_) {
        for (const std::string& account : effects.stop) {
            if (signInHooks_->stop) {
                signInHooks_->stop(account);
            }
        }
        for (const SignInEffects::Start& s : effects.start) {
            if (signInHooks_->start) {
                signInHooks_->start(s.id, s.account);
            }
        }
        for (const SignInEffects::Replay& r : effects.replays) {
            if (signInHooks_->replay) {
                signInHooks_->replay(r.id, r.account, r.target, r.query);
            }
        }
    }
    // A flow that ended unsigned leaves its account needing a fresh one, which a window claims at the person's next
    // show of the overlay: nothing starts without the person there.
    for (const std::string& account : effects.needAgain) {
        if (signedIn_.count(account) == 0 && signsIn(account)) {
            needSignIn(account);
        }
    }
}

void Hub::tellAccount(const std::string& account, const std::string& text) {
    for (SessionId id : store_.ids()) {
        const StoredSession* s = store_.find(id);
        if (s == nullptr || s->info.account != account) {
            continue;
        }
        std::string failed;
        if (store_.commit(id, {SessionChange{entry(EntryKind::Notice, text), {}}}, std::string(), 0, &failed)) {
            refresh(id);
        }
    }
    if (secretEvents_ != nullptr) {
        secretEvents_->logged(SecretLog{0, account, "login", text});
    }
}

void Hub::secretsWith(SecretKeeper* keeper, SecretEvents* events, std::function<bool()> takers,
                      std::function<std::uint64_t()> clock) {
    secretKeeper_ = keeper;
    secretEvents_ = events;
    takers_ = std::move(takers);
    clock_ = std::move(clock);
}

bool Hub::askSecret(const SecretAsker& asker, const std::string& account, const std::string& what,
                    const std::string& why, std::string* refused) {
    auto refuse = [&](const std::string& text) {
        if (refused != nullptr) {
            *refused = text;
        }
        return false;
    };
    if (secretKeeper_ == nullptr) {
        return refuse("this server takes no secrets");
    }
    // The account and the file are names before they reach any text (a refusal, a log line, the dialog): what an AI
    // writes in them cannot forge a line.
    if (!accounts::isName(account) || !accounts::isName(what)) {
        return refuse("the account and the file asked for are each a name (letters, digits, '-', '_' and '.', at most "
                      "64)");
    }
    const accounts::Account* a = accounts_ ? accounts_->account(account) : nullptr;
    if (a == nullptr) {
        return refuse("no account " + account + " is set up in accounts.json (docs/accounts.md)");
    }
    // A friend's AI asks only for an account its friend is linked to; nexus itself asks for any.
    if (asker.session != 0 && !accounts_->linked(asker.name, account)) {
        return refuse("the account " + account + " is not linked to the friend of session " +
                      std::to_string(asker.session) + " in friends.json, so its AI may not ask for its secrets");
    }
    if (a->requiresVerifiedEmail() && what == loginFile(a->provider)) {
        return refuse("Sign in to " + a->email + " in the browser so Nexus can verify the email.");
    }
    const std::vector<std::string> files = secretFiles(a->provider);
    if (std::find(files.begin(), files.end(), what) == files.end()) {
        std::string named;
        for (const std::string& f : files) {
            named += (named.empty() ? "" : ", ") + f;
        }
        return refuse("a " + a->provider + " account has no secret \"" + what + "\"" +
            (files.empty() ? std::string(": its provider documents none") : "; it has " + named));
    }
    if (!isSecretWhy(why)) {
        return refuse("an ask's reason is one line of at most " + std::to_string(kSecretWhyBytes) + " bytes of UTF-8 "
                      "text, with no control, bidirectional or zero-width characters");
    }
    // One open ask per asker, and a few a minute: refused busy at once, before a request is made or an id minted.
    if (asker.session != 0) {
        const std::uint64_t now = clock_ ? clock_() : 0;
        std::deque<std::uint64_t>& times = askTimes_[asker.session];
        while (!times.empty() && times.front() + kSecretAskWindowMs <= now) {
            times.pop_front();
        }
        const bool pending = desk_.waiting(asker.session);
        if (pending || times.size() >= kSecretAsksPerWindow) {
            secretEffects(desk_.refuseAtOnce(asker, account, what, pending ?
                "one of its asks is open already" :
                "it asked " + std::to_string(times.size()) + " times in the last minute"));
            return true;
        }
        times.push_back(now);
    }
    // The id first, durable, so that it is never minted again, across restarts.
    const std::uint64_t id = store_.applied(kSecretIdsClient) + 1;
    std::string failed;
    if (!store_.commit(0, {}, kSecretIdsClient, id, &failed)) {
        return refuse("the request's id was not kept: " + failed);
    }
    secretEffects(desk_.ask(id, asker, account, what, why, takers_ ? takers_() : false, clock_ ? clock_() : 0));
    return true;
}

void Hub::expireSecrets() {
    if (clock_) {
        secretEffects(desk_.expire(clock_()));
    }
}

void Hub::retrySecrets() {
    if (clock_ && secretKeeper_ != nullptr) {
        secretEffects(desk_.retry(clock_(), *secretKeeper_));
    }
}

std::optional<std::uint64_t> Hub::secretWake() const {
    const std::optional<std::uint64_t> a = desk_.deadline();
    const std::optional<std::uint64_t> b = desk_.retryAt();
    if (a && b) {
        return std::min(*a, *b);
    }
    return a ? a : b;
}

std::optional<std::string> Hub::reaskSecrets() {
    while (!reask_.empty()) {
        const auto [account, what] = reask_.front();
        reask_.pop_front();
        // One open or held for the account already serves: nothing to ask.
        if (desk_.busy(account)) {
            continue;
        }
        if (askSecret(SecretAsker{}, account, what, "what was pasted for " + account + "'s " + what +
                      " was not one token: paste it again, alone")) {
            return account;
        }
    }
    return std::nullopt;
}

void Hub::dropSecrets() {
    reask_.clear();
    secretEffects(desk_.drop());
}

// A secret answer or cancel: counted like any action, then given to the desk, which writes the value (never anywhere
// else) or drops it.
bool Hub::secretAction(const Action& action, std::string* why, const std::string& client) {
    if (!count(action, why, client)) {
        return false;
    }
    if (secretKeeper_ == nullptr) {
        if (why != nullptr) {
            *why = describe(action) + " dropped: this server takes no secrets";
        }
        return false;
    }
    if (action.kind == ActionKind::SecretCancel) {
        secretEffects(desk_.cancel(action.request));
        return true;
    }
    secretEffects(desk_.answer(action.request, action.secret, *secretKeeper_, clock_ ? clock_() : 0));
    return true;
}

void Hub::secretEffects(const SecretEffects& effects) {
    for (const SecretNote& n : effects.notes) {
        if (secretEvents_ != nullptr) {
            secretEvents_->noted(n);
        }
        if (n.asker != 0 && store_.find(n.asker) != nullptr) {
            std::string failed;
            if (store_.commit(n.asker, {SessionChange{entry(EntryKind::Notice, n.text), {}}}, std::string(), 0,
                              &failed)) {
                refresh(n.asker);
            }
        }
        if (n.toAccount) {
            noteAccount(n.account, n.text);
        }
    }
    if (effects.changed) {
        state_.setRequests(desk_.open());
    }
    if (secretEvents_ != nullptr) {
        for (const SecretReply& r : effects.replies) {
            secretEvents_->answered(r);
        }
        for (const SecretLog& l : effects.log) {
            secretEvents_->logged(l);
        }
    }
    // nexus's own ask refused for a paste of the wrong shape: the person is asked again (reaskSecrets, next pass).
    for (const SecretReply& r : effects.replies) {
        if (r.asker.session == 0 && !r.stored && r.reason == reason::kMalformed) {
            reask_.emplace_back(r.account, r.what);
        }
    }
    // A login that became durable (at its answer or at a retry) signs its account in, without a restart. Any other ask
    // for that login still open (a paste asked for while the browser's sign-in ran) is answered and closed: nobody is
    // asked to paste a login that is there (models/SignIn.tla NoAskWhenSignedIn).
    for (const SecretRequest& r : effects.stored) {
        const accounts::Account* a = accounts_ ? accounts_->account(r.account) : nullptr;
        if (a != nullptr && !a->requiresVerifiedEmail() && r.what == loginFile(a->provider)) {
            secretEffects(desk_.satisfied(r.account, r.what));
            reportSignedIn(r.account, true);
            if (responder_ != nullptr) {
                responder_->signedIn(r.account);
            }
        }
    }
}

std::string Hub::nextSubscription(SessionId id, const std::vector<std::string>& exhausted, std::string& reason) const {
    reason.clear();
    const auto* session = store_.find(id);
    const auto* who = session && accounts_ ? accounts_->friendNamed(session->info.friendName) : nullptr;
    if (!who) {
        reason = "No subscription roster is available for this session";
        return {};
    }
    const auto& current = session->info.account;
    std::vector<std::string> eligible;
    for (const auto& name : subscriptionsFor(who->name)) {
        if (name != current && signedIn_.contains(name) &&
                std::find(exhausted.begin(), exhausted.end(), name) == exhausted.end())
            eligible.push_back(name);
    }
    const auto next = policy::accountAfterLimit(application_, bundlePath_(), current, who->accounts, eligible, reason);
    if (reason.empty() && next.empty() && !eligible.empty())
        reason = "account_after_limit chose to keep the current account";
    return next;
}

bool Hub::rotate(SessionId id, std::string* why, const std::vector<std::string>& exhausted, bool* noEligible) {
    if (noEligible) *noEligible = false;
    auto refuse = [&](const std::string& what) {
        if (why != nullptr) *why = what;
        return false;
    };
    const StoredSession* s = store_.find(id);
    if (!s) return refuse("no session " + std::to_string(id));
    SessionInfo info = s->info;
    if (!info.autoAccount) {
        if (noEligible) *noEligible = true;
        return refuse("session " + std::to_string(id) + " is pinned to its selected account");
    }
    const std::string was = info.account;
    std::string policyReason;
    const auto next = nextSubscription(id, exhausted, policyReason);
    if (next.empty()) {
        const std::string notice = !policyReason.empty() ? "Account policy: " + policyReason + ". See docs/policy.md." :
            "The account " + was + " reached its usage limit, and " + info.friendName +
            " has no other signed-in account available for this turn. Link another account to " + info.friendName + " in "
            "friends.json, or sign in one it has (docs/accounts.md).";
        std::string failed;
        if (!store_.commit(id, {SessionChange{entry(EntryKind::Notice, notice), {}}}, std::string(), 0, &failed)) {
            return refuse(failed);
        }
        refresh(id);
        if (noEligible) *noEligible = true;
        return refuse("session " + std::to_string(id) + ": " +
            (policyReason.empty() ? info.friendName + " has no other signed-in account" : policyReason));
    }
    info.account = next;
    info.harness = harnessOf(next, info.harness);
    if (!moveTo(id, info, "The account " + was + " reached its usage limit, so this session now runs on the account " +
                next + ", the next signed-in account of " + info.friendName + "'s.")) {
        return refuse("session " + std::to_string(id) + ": the store did not take the move");
    }
    return true;
}

std::vector<std::string> Hub::adoptDefaults() {
    std::vector<std::string> report;
    if (!accounts_) {
        return report;
    }
    for (SessionId id : store_.ids()) {
        SessionInfo info = store_.find(id)->info;
        const accounts::Friend* f = accounts_->friendNamed(info.friendName);
        if (f == nullptr) {
            continue;
        }
        if (!info.account.empty() || f->preferred.empty()) {
            continue;
        }
        info.account = f->preferred;
        info.harness = harnessOf(info.account, info.harness);
        if (!moveTo(id, info, "This session runs on the account " + info.account + ", which the drivers file gave " +
                    info.friendName + " before accounts were set up; accounts.json and friends.json now say so "
                    "(docs/accounts.md).")) {
            break;
        }
        report.push_back("session " + std::to_string(id) + " (" + info.friendName + "): account none -> " +
                         info.account + ", as the drivers file ran it");
    }
    return report;
}

// An account action: the registry it makes, kept by the keeper, then taken, then the sessions settled; refused and
// counted, changing nothing, when it does not fit the registry the hub holds.
bool Hub::accountAction(const Action& action, std::string* why, const std::string& client) {
    if (!accounts_) {
        return counted(action, "no account action is taken: this server holds no accounts (it was started without "
            "its config repository)", why, client);
    }
    accounts::Registry next = *accounts_;
    const std::string& a = action.account;
    const std::string& f = action.friendName;
    accounts::Friend* fr = next.friendNamed(f);
    std::string refused;
    switch (action.kind) {
        case ActionKind::AddAccount:
            if (!accounts::isName(a) || !accounts::isProvider(action.text)) {
                refused = "an account is set up with a name (1 to 64 letters, digits, '-', '_' or '.', not starting "
                    "with '.' or '-') and a provider (lowercase, as claude): \"" + a + "\" of \"" + action.text +
                    "\" is not";
            } else if (next.account(a) != nullptr) {
                refused = "the account " + a + " is already set up";
            } else {
                next.accounts.push_back(accounts::Account{a, action.text});
            }
            break;
        case ActionKind::RemoveAccount:
            if (next.account(a) == nullptr) {
                refused = "no account " + a + " is set up";
            } else {
                next.accounts.erase(std::find_if(next.accounts.begin(), next.accounts.end(),
                    [&](const accounts::Account& x) { return x.name == a; }));
                for (accounts::Friend& x : next.friends) {
                    x.accounts.erase(std::remove(x.accounts.begin(), x.accounts.end(), a), x.accounts.end());
                    if (x.preferred == a) {
                        x.preferred.clear();
                    }
                }
            }
            break;
        case ActionKind::AddFriend:
            if (!accounts::isName(f)) {
                refused = "a friend's name is 1 to 64 letters, digits, '-', '_' or '.', not starting with '.' or '-': \"" +
                    f + "\" is not";
            } else if (fr != nullptr) {
                refused = "the friend " + f + " is already set up";
            } else if (next.friends.size() >= accounts::kMaxFriends) {
                refused = "friends.json holds " + std::to_string(next.friends.size()) + " friends, the most it may "
                    "(" + std::to_string(accounts::kMaxFriends) + "); take one out to add " + f;
            } else {
                next.friends.push_back(accounts::Friend{f, {}, {}});
            }
            break;
        case ActionKind::LinkAccount:
            if (fr == nullptr) {
                refused = "no friend " + f + " is set up";
            } else if (next.account(a) == nullptr) {
                refused = "no account " + a + " is set up";
            } else if (std::find(fr->accounts.begin(), fr->accounts.end(), a) != fr->accounts.end()) {
                refused = f + " is already linked to " + a;
            } else if (!fr->limit && next.account(a)->billing == accounts::Billing::Api) {
                refused = a + " is billed at API rate, and a friend on an API-billed account is always limited, but " +
                    f + " has \"limit\": false in friends.json; set it to true there first (docs/accounts.md)";
            } else {
                fr->accounts.push_back(a);
            }
            break;
        case ActionKind::UnlinkAccount:
            if (fr == nullptr || std::find(fr->accounts.begin(), fr->accounts.end(), a) == fr->accounts.end()) {
                refused = f + " is not linked to " + a;
            } else {
                fr->accounts.erase(std::remove(fr->accounts.begin(), fr->accounts.end(), a), fr->accounts.end());
                if (fr->preferred == a) {
                    fr->preferred.clear();
                }
            }
            break;
        default:    // SetDefault
            if (fr == nullptr) {
                refused = "no friend " + f + " is set up";
            } else if (!a.empty() && std::find(fr->accounts.begin(), fr->accounts.end(), a) == fr->accounts.end()) {
                refused = a + " is not one of " + f + "'s accounts: link it first";
            } else {
                fr->preferred = a;
            }
            break;
    }
    if (!refused.empty()) {
        return counted(action, "the accounts were not changed: " + refused, why, client);
    }
    if (auto problem = accountRosterProblem(next); !problem.empty()) {
        return counted(action, problem, why, client);
    }
    const std::vector<std::string> lines = accounts::changes(*accounts_, next);
    std::string message;
    for (const std::string& l : lines) {
        message += (message.empty() ? "" : "; ") + l;
    }
    message += client.empty() ? std::string(" (asked by an action)") : " (asked by the window " + client + ")";
    std::string failed;
    if (keeper_ != nullptr && !keeper_->keep(next, message, &failed)) {
        return counted(action, "the accounts were not changed: " + failed, why, client);
    }
    const accounts::Registry before = *accounts_;
    take(std::move(next));
    // Committed and taken: a store that fails now stops the server, which settles the sessions at its next start.
    if (!count(action, why, client) || !settle(nullptr, message, &before)) {
        if (why != nullptr) {
            *why = "the accounts were changed and committed (" + message + "), but the store failed while the "
                "action was counted or the sessions moved; the server stops, and moves them at its next start";
        }
        return false;
    }
    return true;
}

bool Hub::setActivity(SessionId id, Activity activity) {
    if (store_.find(id) == nullptr) {
        return false;
    }
    auto it = activity_.find(id);
    const Activity was = it == activity_.end() ? Activity::Idle : it->second;
    if (was == activity) {
        return false;
    }
    if (activity == Activity::Idle) {
        activity_.erase(it);
    } else {
        activity_[id] = activity;
    }
    refresh(id);
    return true;
}

// Compute the same offer for display and acceptance. An account can sign out or a fork
// can change its policy between those two calls.
void Hub::offerLimitChoice(SessionId id, const DriverState& driver, SessionInfo& info) {
    info.limitRequest = driver.nextTurn;
    if (info.harness != "claude")
        return;
    std::string reason;
    info.limitAccount = nextSubscription(id, driver.exhaustedAccounts, reason);
    if (accounts_ && (!accounts_->linked(info.friendName, info.account) || !signedIn_.count(info.account)))
        return;
    auto model = policy::modelAfterLimit(application_, bundlePath_(), info.friendName, info.account, info.model);
    if (!model.empty() && model != info.model && model.size() <= limits_.setting && printable(model))
        info.limitModel = std::move(model);
}

bool Hub::resolveLimit(const Action& action, std::string* why, const std::string& client) {
    const auto* session = store_.find(action.session);
    if (!session || !session->driver.modelLimitPending || action.request != session->driver.nextTurn ||
            action.account != session->info.account)
        return counted(action, "This model-limit choice is no longer pending.", why, client);

    SessionInfo info = session->info;
    DriverState driver = session->driver;
    SessionInfo offer = info;
    offerLimitChoice(action.session, driver, offer);
    const bool account = action.text == "account";
    const std::string& target = account ? offer.limitAccount : offer.limitModel;
    if ((!account && action.text != "model") || target.empty() || action.friendName != target) {
        refresh(action.session);
        return counted(action, "That model-limit option is no longer available.", why, client);
    }

    std::vector<SessionChange> changes;
    if (account) {
        info.account = target;
        changes.push_back(SessionChange{switchNotice(target), {}});
    } else {
        info.model = target;
        std::erase(driver.exhaustedAccounts, info.account);
        changes.push_back(SessionChange{entry(EntryKind::Notice, "Switched to model " + target), {}});
    }
    driver.modelLimitPending = false;
    changes.push_back(infoChange(info));
    changes.push_back(SessionChange{nullptr, {}, std::move(driver)});
    if (!store_.commit(action.session, changes, client, action.num, why))
        return false;
    refresh(action.session);
    if (responder_)
        responder_->configured(action.session, info);
    return true;
}

// The session's new tail, in place of its old one; every other session is shared with the state before.
void Hub::refresh(SessionId id) {
    const StoredSession* stored = store_.find(id);
    auto open = open_.find(id);
    SessionView view = tailOf(id, stored->info, stored->transcript, stored->queue, limits_.tail,
        open == open_.end() ? nullptr : open->second);
    if (stored->driver.modelLimitPending)
        offerLimitChoice(id, stored->driver, view.info);
    auto activity = activity_.find(id);
    view.activity = activity == activity_.end() ? Activity::Idle : activity->second;
    auto fresh = std::make_shared<const SessionView>(std::move(view));
    std::vector<SessionRef> sessions = state_.sessions();
    auto it = std::lower_bound(sessions.begin(), sessions.end(), id,
        [](const SessionRef& s, SessionId n) { return s->id < n; });
    if (it != sessions.end() && (*it)->id == id) {
        *it = fresh;
    } else {
        sessions.insert(it, fresh);
    }
    // The friends as they were: one list, shared by every state until the registry's friends change (stateFriends).
    Sessions next(std::move(sessions));
    next.setRequests(desk_.open());
    next.shareFriends(state_);
    next.setOffers(publicOffers());
    state_ = std::move(next);
}

// The friends of the registry the hub holds, in its order, into the state the windows receive: who the friends are is
// the server's, and a window shows a tab for each (sync/client.h applyFriends).
void Hub::stateFriends() {
    auto friends = windowRoster(*accounts_, indexAccounts(*accounts_), &signedIn_,
                                signInHooks_.has_value() && secretKeeper_ != nullptr);
    if (!state_.friendsKnown() || state_.friends() != friends) {
        state_.setFriends(std::move(friends));
    }
    for (SessionId id : store_.ids()) {
        if (store_.find(id)->driver.modelLimitPending)
            refresh(id);
    }
}

} // namespace nexus::sync
