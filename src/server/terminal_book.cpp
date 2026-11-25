#include "server/terminal_book.h"

#include <algorithm>
#include <stdexcept>

namespace nexus::server {

namespace {

std::string named(TerminalId id) {
    return "terminal " + std::to_string(id);
}

std::uint32_t bounded(std::uint32_t value, std::uint32_t fallback, std::uint32_t most) {
    return value == 0 ? fallback : std::min(value, most);
}

} // namespace

TerminalBook::TerminalBook(std::uint32_t epoch) : epoch_(epoch) {
    if (epoch == 0) {
        throw std::logic_error("TerminalBook: epoch 0");
    }
}

const TerminalBook::Entry* TerminalBook::find(TerminalId id) const {
    const auto it = terminals_.find(id);
    return it == terminals_.end() ? nullptr : &it->second;
}

TerminalOutcome TerminalBook::apply(const sync::Action& action, bool owner) {
    TerminalOutcome out;
    if (!sync::isTerminal(action.kind)) {
        out.refused = sync::describe(action) + " is not a terminal's";
        return out;
    }
    if (!owner) {
        out.refused = "a terminal runs a shell as the server's owner, and this link was not made by its owner";
        return out;
    }
    if (action.kind == sync::ActionKind::TerminalCreate) {
        if (terminals_.size() >= sync::kMaxTerminals) {
            out.refused = "this server runs at most " + std::to_string(sync::kMaxTerminals) +
                " terminals; close one first";
            return out;
        }
        if (next_ == 0) {   // 2^32 terminals made in one run
            out.refused = "this server has made all the terminals one run can; restart it";
            return out;
        }
        const TerminalId id = (TerminalId(epoch_) << 32) | next_++;
        Entry e;
        e.opener = action.text;
        e.directory = action.account;
        e.columns = bounded(action.columns, kDefaultTerminalColumns, sync::kMaxTerminalColumns);
        e.rows = bounded(action.rows, kDefaultTerminalRows, sync::kMaxTerminalRows);
        out.effect = {TerminalEffect::Kind::Spawn, id, e.directory, e.columns, e.rows};
        terminals_.emplace(id, std::move(e));
        return out;
    }
    const auto it = terminals_.find(action.terminal);
    if (it == terminals_.end()) {
        out.refused = (action.terminal >> 32) != epoch_ && action.terminal != 0 ?
            named(action.terminal) + " ended when the server restarted" : "there is no " + named(action.terminal);
        return out;
    }
    Entry& e = it->second;
    switch (action.kind) {
        case sync::ActionKind::TerminalInput:
            if (e.state != State::Running) {
                out.refused = named(action.terminal) + "'s shell has ended; its input was not sent";
            } else if (action.secret.size() > sync::kMaxTerminalInput) {
                out.refused = "input of " + std::to_string(action.secret.size()) + " bytes to " +
                    named(action.terminal) + " is more than one action carries";
            } else if (!action.secret.empty()) {
                out.effect = {TerminalEffect::Kind::Write, action.terminal, {}, 0, 0};
            }
            return out;
        case sync::ActionKind::TerminalResize:
            e.columns = bounded(action.columns, e.columns, sync::kMaxTerminalColumns);
            e.rows = bounded(action.rows, e.rows, sync::kMaxTerminalRows);
            out.effect = {TerminalEffect::Kind::Resize, action.terminal, {}, e.columns, e.rows};
            return out;
        case sync::ActionKind::TerminalClose:
            if (e.state == State::Running) {
                out.effect = {TerminalEffect::Kind::Kill, action.terminal, {}, 0, 0};
            }
            terminals_.erase(it);
            return out;
        default:
            out.refused = sync::describe(action) + " is not a terminal's";
            return out;
    }
}

void TerminalBook::ended(TerminalId id, std::string how) {
    const auto it = terminals_.find(id);
    if (it == terminals_.end() || it->second.state == State::Ended) {
        return;
    }
    it->second.state = State::Ended;
    it->second.ended = how.empty() ? std::string("ended") : std::move(how);
}

std::string endedLine(bool signaled, int code) {
    return signaled ? "ended by signal " + std::to_string(code) : "exited with code " + std::to_string(code);
}

} // namespace nexus::server
