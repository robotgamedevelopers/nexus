#include "sync/actions.h"

#include <stdexcept>

#include "sync/wire.h"

namespace nexus::sync {

namespace {

// A diff: the number of its first action, how many follow, and each action's kind, session, friend and text. The
// numbers of the rest follow from the first, so a diff is always one unbroken run.
std::string encode(std::uint64_t start, const std::vector<Action>& held, std::size_t from) {
    Writer w;
    w.u64(start);
    w.u64(held.size() - from);
    for (std::size_t i = from; i < held.size(); ++i) {
        w.byte(static_cast<std::uint8_t>(held[i].kind));
        w.u64(held[i].session);
        w.str(held[i].friendName);
        w.str(held[i].text);
    }
    return w.take();
}

} // namespace

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
    return encode(existing.next(), *held_, static_cast<std::size_t>(existing.next() - first_));
}

std::string Actions::init_diff() const {
    return held_->empty() ? std::string() : encode(first_, *held_, 0);
}

std::vector<Action> Actions::parse(const std::string& diff) {
    std::vector<Action> out;
    if (diff.empty())
        return out;
    Reader r(diff);
    const std::uint64_t start = r.u64();
    const std::uint64_t count = r.u64();
    if (start == 0)
        throw BadDiff("actions diff starts at number 0; numbers start at 1");
    if (count == 0 || count > diff.size())
        throw BadDiff("actions diff says it holds " + std::to_string(count) + " actions in " +
            std::to_string(diff.size()) + " bytes");
    if (start + count < start)
        throw BadDiff("actions diff numbers run past 64 bits");
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        Action a;
        a.num = start + i;
        const std::uint8_t kind = r.byte();
        if (kind < static_cast<std::uint8_t>(ActionKind::Send) ||
                kind > static_cast<std::uint8_t>(ActionKind::SetEffort)) {
            throw BadDiff("actions diff holds action kind " + std::to_string(kind) + ", none of send (1) to set "
                "effort (8)");
        }
        a.kind = static_cast<ActionKind>(kind);
        a.session = r.u64();
        a.friendName = r.str();
        a.text = r.str();
        out.push_back(std::move(a));
    }
    r.end();
    return out;
}

void Actions::apply_string(const std::string& diff) {
    const std::vector<Action> actions = parse(diff);
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
