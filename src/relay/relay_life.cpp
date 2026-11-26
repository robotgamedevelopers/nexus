#include "relay/relay_life.h"

#include <algorithm>
#include <utility>

namespace nexus::relay {

std::uint64_t RelayLife::send(const sync::Action& action) {
    Out o;
    o.action = true;
    o.number = ++sent_;
    o.bytes = client_.send(action);
    out_.push_back(std::move(o));
    return sent_;
}

void RelayLife::history(sync::SessionId id, std::uint64_t epoch, std::uint64_t before) {
    Out o;
    o.bytes = client_.history(id, epoch, before);
    out_.push_back(std::move(o));
}

bool RelayLife::poll(LinkState* out) {
    const bool shown = client_.poll(out);
    if (!shown && !changed_) return false;
    if (!shown) {
        // Only what this end holds moved: the rest of the state is the latest the client showed.
        LinkState latest;
        client_.peek(&latest);
        *out = std::move(latest);
    }
    // The window's next action the server has not applied, in the window's numbers (the relay's state has it in its
    // link's), where the state's format has one at all.
    if (out->sessions.nextAction() != 0) out->sessions.setNextAction(acked_ + 1);
    out->cut = std::move(untold_);
    untold_.clear();
    changed_ = false;
    return true;
}

void RelayLife::stop() {
    stopped_ = true;
    for (Out& o : out_) wipe(&o.bytes);
    out_.clear();
    helloOwed_ = false;
}

RelayLife::Phase RelayLife::phase() const {
    if (stopped_) return Phase::Stopped;
    if (!live_) return Phase::Waiting;
    return client_.helloRead() ? Phase::Running : Phase::Hello;
}

bool RelayLife::mayStart(LifeMs now) const {
    return !stopped_ && !live_ && now >= dueAt_;
}

LifeMs RelayLife::waitMs(LifeMs now) const {
    if (stopped_ || live_) return kLastBackoffMs;
    return now >= dueAt_ ? 0 : dueAt_ - now;
}

LifeMs RelayLife::leftMs(LifeMs now) const {
    return phase() == Phase::Waiting ? waitMs(now) : 0;
}

bool RelayLife::started(LifeMs now) {
    if (!mayStart(now)) return false;
    live_ = true;
    ++relays_;
    startedAt_ = now;
    base_ = written_;
    // The new relay's link numbers its actions from 1: the first written to it is the window's base + 1.
    client_.open();
    helloOwed_ = true;
    return true;
}

bool RelayLife::wantsWrite() const {
    return live_ && !stopped_ && (helloOwed_ || !out_.empty());
}

bool RelayLife::take(std::string* bytes) {
    if (!wantsWrite()) return false;
    if (helloOwed_) {
        helloOwed_ = false;
        *bytes = helloFrame();
        return true;
    }
    Out o = std::move(out_.front());
    out_.pop_front();
    if (o.action) written_ = o.number;
    *bytes = std::move(o.bytes);
    return true;
}

bool RelayLife::feed(const char* data, std::size_t size, std::string* why) {
    if (!live_ || stopped_) return true;
    const bool ok = client_.feed(data, size, why);
    const std::uint64_t next = client_.nextAction();
    if (next != 0) {
        // The server applied the relay's actions before `next`: the window's up to base + next - 1, never past what
        // was written to it (a relay that says more is not believed beyond it).
        const std::uint64_t applied = std::min(base_ + next - 1, written_);
        if (applied > acked_) {
            acked_ = applied;
            changed_ = true;
        }
    }
    return ok;
}

void RelayLife::ended(const std::string& why, LifeMs now) {
    if (!live_) return;
    live_ = false;
    helloOwed_ = false;
    if (stopped_) return;
    for (std::uint64_t n = std::max(base_, acked_) + 1; n <= written_; ++n) {
        cut_.push_back(n);
        untold_.push_back(n);
    }
    std::string problem = why;
    const std::size_t lost = written_ > std::max(base_, acked_) ? written_ - std::max(base_, acked_) : 0;
    if (lost > 0) {
        problem += "; " + std::to_string(lost) + (lost == 1 ? " action" : " actions") +
            " the server had not acknowledged went with it, not sent again";
    }
    client_.ended(problem);
    changed_ = true;
    const LifeMs wait = now - startedAt_ >= kRanWellMs ? kFirstBackoffMs : backoff_;
    dueAt_ = now + wait;
    backoff_ = std::min(wait * 2, kLastBackoffMs);
}

} // namespace nexus::relay
