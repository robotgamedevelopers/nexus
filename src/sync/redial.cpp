#include "sync/redial.h"

#include <algorithm>

#include "sync/client.h"

namespace nexus::sync {

RedialMs redialBackoffMs(unsigned attempts) {
    if (attempts == 0) {
        return 0;
    }
    RedialMs wait = kRedialFirstMs;
    for (unsigned i = 1; i < attempts && wait < kRedialMaxMs; ++i) {
        wait *= 2;
    }
    return std::min(wait, kRedialMaxMs);
}

void Redial::attempted(RedialMs now) {
    if (attempts_ < ~0u) {
        ++attempts_;
    }
    nextAt_ = now + redialBackoffMs(attempts_);
}

void Redial::dialed(RedialMs now) {
    session_ = true;
    heard_ = false;
    ended_ = false;
    heardAt_ = now;
    attempted(now);
}

void Redial::failed(RedialMs now) {
    attempted(now);
}

void Redial::heard(RedialMs at) {
    if (!session_ || ended_ || at < heardAt_ || (heard_ && at == heardAt_)) {
        return;
    }
    heard_ = true;
    heardAt_ = at;
    attempts_ = 0;
    nextAt_ = 0;
    everHeard_ = true;
}

void Redial::ended() {
    if (session_) {
        ended_ = true;
    }
}

bool Redial::quiet(RedialMs now) const {
    return session_ && now >= heardAt_ && now - heardAt_ >= kQuietMs;
}

bool Redial::lost(RedialMs now) const {
    return !session_ || ended_ || quiet(now);
}

RedialMs Redial::waitMs(RedialMs now) const {
    const RedialMs lostAt = lost(now) ? now : heardAt_ + kQuietMs;
    const RedialMs at = std::max(lostAt, nextAt_);
    return at > now ? at - now : 0;
}

} // namespace nexus::sync
