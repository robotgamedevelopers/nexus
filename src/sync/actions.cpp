#include "sync/actions.h"

#include <stdexcept>

#include "sync/codec.h"
#include "sync/wire.h"

namespace nexus::sync {

namespace {

bool isSecret(ActionKind kind) {
    return kind == ActionKind::SecretAnswer || kind == ActionKind::SecretCancel;
}

bool isSignIn(ActionKind kind) {
    return kind == ActionKind::SignInClaim || kind == ActionKind::SignInOpened || kind == ActionKind::SignInCallback;
}

// A diff: the number of its first action and the actions from there, one unbroken run, in the link's format
// (sync/codec.h).
std::string encode(std::uint64_t start, const std::vector<Action>& held, std::size_t from, std::uint32_t format) {
    if (format >= kTablesFormat) {
        return codec::writeActionsTables(start, held, from, format);
    }
    return codec::writeActionsLegacy(start, held, from, format);
}

const char* kindName(ActionKind kind) {
    switch (kind) {
        case ActionKind::Unknown: return "unknown";
        case ActionKind::Send: return "send";
        case ActionKind::Queue: return "queue";
        case ActionKind::Stop: return "stop";
        case ActionKind::Create: return "create";
        case ActionKind::Ensure: return "ensure";
        case ActionKind::SetAccount: return "set-account";
        case ActionKind::SetModel: return "set-model";
        case ActionKind::SetEffort: return "set-effort";
        case ActionKind::AddAccount: return "add-account";
        case ActionKind::RemoveAccount: return "remove-account";
        case ActionKind::AddFriend: return "add-friend";
        case ActionKind::LinkAccount: return "link-account";
        case ActionKind::UnlinkAccount: return "unlink-account";
        case ActionKind::SetDefault: return "set-default";
        case ActionKind::SecretAnswer: return "secret-answer";
        case ActionKind::SecretCancel: return "secret-cancel";
        case ActionKind::SignInClaim: return "sign-in-claim";
        case ActionKind::SignInOpened: return "sign-in-opened";
        case ActionKind::SignInCallback: return "sign-in-callback";
    }
    return "unknown";
}

} // namespace

std::string describe(const Action& action) {
    std::string out = "action " + std::to_string(action.num) + " (" + kindName(action.kind);
    if (isSecret(action.kind)) {
        out += " for secret request " + std::to_string(action.request);
    } else if (isSignIn(action.kind)) {
        out += " for sign-in " + std::to_string(action.request);
    }
    return out + ")";
}

Actions::Actions() : held_(std::make_shared<const std::vector<Action>>()) {}

Actions::Actions(std::uint64_t next) : first_(next == 0 ? 1 : next), held_(std::make_shared<const std::vector<Action>>()) {}

std::uint64_t Actions::add(Action action) {
    auto held = std::make_shared<std::vector<Action>>(*held_);
    const std::uint64_t num = next();
    action.num = num;
    held->push_back(std::move(action));
    held_ = std::move(held);
    return num;
}

std::string Actions::diff_from(const Actions& existing) const {
    if (existing.next() > next())
        throw std::logic_error("Actions::diff_from: the state diffed from is ahead of this one (" +
            std::to_string(existing.next()) + " > " + std::to_string(next()) + ")");
    if (existing.next() == next())
        return std::string();
    if (existing.next() < first_)
        throw std::logic_error("Actions::diff_from: this state no longer holds actions " +
            std::to_string(existing.next()) + " to " + std::to_string(first_ - 1) + " that the state diffed from lacks");
    return encode(existing.next(), *held_, static_cast<std::size_t>(existing.next() - first_), format_);
}

std::string Actions::init_diff() const {
    return held_->empty() ? std::string() : encode(first_, *held_, 0, format_);
}

std::vector<Action> Actions::parse(const std::string& diff, std::uint32_t format, int* unknown) {
    if (unknown != nullptr) {
        *unknown = 0;
    }
    if (diff.empty())
        return std::vector<Action>();
    if (format >= kTablesFormat) {
        int skipped = 0;
        std::vector<Action> out = codec::readActionsTables(diff, format, &skipped);
        if (unknown != nullptr) {
            *unknown = skipped;
        }
        return out;
    }
    return codec::readActionsLegacy(diff, format);
}

void Actions::apply_string(const std::string& diff) {
    if (closed_ && *closed_) {
        throw BadDiff("the session is shutting down");
    }
    const std::vector<Action> actions = parse(diff, format_);
    if (actions.empty())
        return;
    if (actions.front().num > next())
        throw BadDiff("actions diff starts at " + std::to_string(actions.front().num) + " but this state ends at " +
            std::to_string(next() - 1) + ": actions in between are missing");
    if (actions.back().num < next())
        return;
    auto held = std::make_shared<std::vector<Action>>(*held_);
    std::uint64_t want = next();
    for (const Action& a : actions) {
        if (a.num == want) {
            held->push_back(a);
            ++want;
        }
    }
    held_ = std::move(held);
}

bool Actions::operator==(const Actions& x) const {
    return first_ == x.first_ && (held_ == x.held_ || *held_ == *x.held_);
}

void Actions::subtract(const Actions* prefix) {
    const std::uint64_t upto = prefix->next();  // read before a subtraction of this state from itself changes it
    if (upto <= first_)
        return;
    if (upto >= next()) {
        first_ = next();
        held_ = std::make_shared<const std::vector<Action>>();
        return;
    }
    const auto drop = static_cast<std::ptrdiff_t>(upto - first_);
    held_ = std::make_shared<const std::vector<Action>>(held_->begin() + drop, held_->end());
    first_ = upto;
}

std::vector<Action> Applier::apply(const std::vector<Action>& actions, std::string* refused) {
    std::vector<Action> fresh;
    if (actions.empty())
        return fresh;
    if (actions.front().num > applied_ + 1) {
        if (refused != nullptr)
            *refused = "actions " + std::to_string(actions.front().num) + " to " + std::to_string(actions.back().num) +
                " arrived after action " + std::to_string(applied_) + ": " +
                std::to_string(actions.front().num - applied_ - 1) + " in between are missing, so none were applied";
        return fresh;
    }
    for (const Action& a : actions) {
        if (a.num == applied_ + 1) {
            fresh.push_back(a);
            ++applied_;
        }
    }
    return fresh;
}

} // namespace nexus::sync
