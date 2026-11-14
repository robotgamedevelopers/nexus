#include "sync/secrets.h"

#include <algorithm>

namespace nexus::sync {

// Through a volatile pointer, so that the writes are not optimised away.
void wipeBytes(std::string& s) {
    s.resize(s.capacity());
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); ++i) {
        p[i] = 0;
    }
    s.clear();
}

namespace {

void wipe(std::string& s) {
    wipeBytes(s);
}

std::string note(const SecretRequest& r, const std::string& outcome) {
    return "secret request " + std::to_string(r.id) + ": " + r.account + "'s " + r.what + " " + outcome;
}

} // namespace

const char* const kSecretIdsClient = "nexus:secret-requests";

SecretValue::SecretValue(SecretValue&& other) noexcept : bytes_(other.bytes_) {
    wipe(other.bytes_);
}

SecretValue& SecretValue::operator=(const SecretValue& other) {
    if (this != &other) {
        wipe(bytes_);
        bytes_ = other.bytes_;
    }
    return *this;
}

SecretValue& SecretValue::operator=(SecretValue&& other) noexcept {
    if (this != &other) {
        wipe(bytes_);
        bytes_ = other.bytes_;
        wipe(other.bytes_);
    }
    return *this;
}

SecretValue::~SecretValue() {
    wipe(bytes_);
}

void SecretValue::clear() {
    wipe(bytes_);
}

std::vector<std::string> secretFiles(const std::string& provider) {
    if (provider == "claude") {
        return {"oauth-token"};
    }
    return {};
}

std::string loginFile(const std::string& provider) {
    return provider == "claude" ? std::string("oauth-token") : std::string();
}

bool isSecretWhy(std::string_view why) {
    if (why.size() > kSecretWhyBytes) {
        return false;
    }
    std::size_t i = 0;
    while (i < why.size()) {
        const unsigned char b = static_cast<unsigned char>(why[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (b < 0x80) {
            n = 1;
            cp = b;
        } else if (b >= 0xC2 && b <= 0xDF) {
            n = 2;
            cp = b & 0x1Fu;
        } else if (b >= 0xE0 && b <= 0xEF) {
            n = 3;
            cp = b & 0x0Fu;
        } else if (b >= 0xF0 && b <= 0xF4) {
            n = 4;
            cp = b & 0x07u;
        } else {
            return false;
        }
        if (i + n > why.size()) {
            return false;
        }
        for (std::size_t k = 1; k < n; ++k) {
            const unsigned char c = static_cast<unsigned char>(why[i + k]);
            if ((c & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        // Overlong forms, surrogates and code points past U+10FFFF are not UTF-8.
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        const bool control = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
        const bool bidi = cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
            (cp >= 0x2066 && cp <= 0x2069);
        const bool zeroWidth = (cp >= 0x200B && cp <= 0x200D) || cp == 0x2060 || cp == 0xFEFF;
        const bool separator = cp == 0x2028 || cp == 0x2029;
        if (control || bidi || zeroWidth || separator) {
            return false;
        }
        i += n;
    }
    return true;
}

std::string SecretLog::line() const {
    return "secret request " + std::to_string(id) + " for " + account + ": " + event;
}

SecretEffects SecretDesk::ask(std::uint64_t id, const SecretAsker& asker, const std::string& account,
                              const std::string& what, const std::string& why, bool takers, std::uint64_t now) {
    SecretEffects out;
    SecretRequest r{id, account, what, asker.session, asker.name, why};
    const bool busy = std::any_of(open_.begin(), open_.end(),
        [&](const SecretRequest& o) { return o.account == account; });
    if (busy || !takers) {
        const std::string why2 = busy ? reason::kBusy : reason::kNoClient;
        out.replies.push_back(SecretReply{id, asker, false, why2});
        out.notes.push_back(SecretNote{id, asker.session, "refused", account,
            note(r, busy ? "was not asked for: a request for it is open already" :
                           "was not asked for: no window that can take it is connected")});
        out.log.push_back(SecretLog{id, account, why2});
        remember(id, SecretState::Refused);
        return out;
    }
    open_.push_back(r);
    askers_[id] = asker;
    due_[id] = now + kSecretDeadlineMs;
    out.notes.push_back(SecretNote{id, asker.session, "asked", account, note(r, "was asked for in nexus's window")});
    out.log.push_back(SecretLog{id, account, "asked"});
    out.changed = true;
    return out;
}

SecretEffects SecretDesk::refuseAtOnce(const SecretAsker& asker, const std::string& account, const std::string& what,
                                       const std::string& why) const {
    SecretEffects out;
    out.replies.push_back(SecretReply{0, asker, false, reason::kBusy});
    out.notes.push_back(SecretNote{0, asker.session, "refused", account,
        "secret ask: " + account + "'s " + what + " was not asked for: " + why});
    out.log.push_back(SecretLog{0, account, std::string(reason::kBusy) + " (not asked: session " +
                                std::to_string(asker.session) + ": " + why + ")"});
    return out;
}

void SecretDesk::end(std::size_t index, SecretState state, SecretEffects& out) {
    const std::uint64_t id = open_[index].id;
    open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(index));
    askers_.erase(id);
    due_.erase(id);
    remember(id, state);
    out.changed = true;
}

void SecretDesk::remember(std::uint64_t id, SecretState state) {
    ended_[id] = state;
    while (ended_.size() > kKeptEnded) {
        ended_.erase(ended_.begin());
    }
}

SecretEffects SecretDesk::answer(std::uint64_t id, const SecretValue& value, SecretKeeper& keeper) {
    SecretEffects out;
    auto it = std::find_if(open_.begin(), open_.end(), [&](const SecretRequest& r) { return r.id == id; });
    if (it == open_.end()) {
        // Answered in another window, cancelled, past its deadline, or from before a restart: nothing is stored.
        out.log.push_back(SecretLog{id, std::string(), "late"});
        return out;
    }
    const SecretRequest r = *it;
    const SecretAsker asker = askers_[id];
    std::string why;
    const Kept kept = keeper.keep(r.account, r.what, value, &why);
    if (kept == Kept::Durable) {
        out.replies.push_back(SecretReply{id, asker, true, std::string()});
        out.notes.push_back(SecretNote{id, r.asker, "stored", r.account, note(r, "was stored")});
        out.log.push_back(SecretLog{id, r.account, echoFault_ ? "stored " + value.bytes() : std::string("stored")});
        end(static_cast<std::size_t>(it - open_.begin()), SecretState::Stored, out);
    } else if (kept == Kept::Unconfirmed) {
        // The value is in its file: stored, and said so with what is true of it.
        out.replies.push_back(SecretReply{id, asker, true, reason::kUnconfirmed});
        out.notes.push_back(SecretNote{id, r.asker, "stored", r.account,
            note(r, "was stored, but the disk did not confirm it durable; see server.log")});
        out.log.push_back(SecretLog{id, r.account, std::string("stored, unconfirmed: ") + why});
        end(static_cast<std::size_t>(it - open_.begin()), SecretState::Stored, out);
    } else {
        out.replies.push_back(SecretReply{id, asker, false, reason::kUnwritable});
        out.notes.push_back(SecretNote{id, r.asker, "refused", r.account,
            note(r, "was not stored: its file could not be written; see server.log")});
        out.log.push_back(SecretLog{id, r.account, std::string(reason::kUnwritable) + ": " + why});
        end(static_cast<std::size_t>(it - open_.begin()), SecretState::Refused, out);
    }
    return out;
}

SecretEffects SecretDesk::cancel(std::uint64_t id) {
    SecretEffects out;
    auto it = std::find_if(open_.begin(), open_.end(), [&](const SecretRequest& r) { return r.id == id; });
    if (it == open_.end()) {
        out.log.push_back(SecretLog{id, std::string(), "late"});
        return out;
    }
    const SecretRequest r = *it;
    out.replies.push_back(SecretReply{id, askers_[id], false, reason::kCancelled});
    out.notes.push_back(SecretNote{id, r.asker, "refused", r.account, note(r, "was not stored: the person cancelled")});
    out.log.push_back(SecretLog{id, r.account, reason::kCancelled});
    end(static_cast<std::size_t>(it - open_.begin()), SecretState::Refused, out);
    return out;
}

SecretEffects SecretDesk::expire(std::uint64_t now) {
    SecretEffects out;
    for (std::size_t i = 0; i < open_.size();) {
        const SecretRequest r = open_[i];
        if (due_[r.id] > now) {
            ++i;
            continue;
        }
        out.replies.push_back(SecretReply{r.id, askers_[r.id], false, reason::kTimeout});
        out.notes.push_back(SecretNote{r.id, r.asker, "refused", r.account,
            note(r, "was not stored: nobody answered within 10 minutes")});
        out.log.push_back(SecretLog{r.id, r.account, reason::kTimeout});
        end(i, SecretState::Refused, out);
    }
    return out;
}

SecretEffects SecretDesk::drop() {
    SecretEffects out;
    while (!open_.empty()) {
        out.log.push_back(SecretLog{open_.front().id, open_.front().account, "dropped"});
        end(0, SecretState::Dropped, out);
    }
    return out;
}

std::optional<std::uint64_t> SecretDesk::deadline() const {
    std::optional<std::uint64_t> at;
    for (const auto& [id, due] : due_) {
        if (!at || due < *at) {
            at = due;
        }
    }
    return at;
}

std::optional<SecretState> SecretDesk::stateOf(std::uint64_t id) const {
    if (isOpen(id)) {
        return SecretState::Open;
    }
    auto it = ended_.find(id);
    if (it == ended_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool SecretDesk::isOpen(std::uint64_t id) const {
    return std::any_of(open_.begin(), open_.end(), [&](const SecretRequest& r) { return r.id == id; });
}

bool SecretPrompt::received(const std::vector<SecretRequest>& open) {
    const std::optional<std::uint64_t> before = shown_ ? std::optional<std::uint64_t>(shown_->id) : std::nullopt;
    view_ = open;
    if (shown_) {
        auto it = std::find_if(view_.begin(), view_.end(), [&](const SecretRequest& r) { return r.id == shown_->id; });
        if (it != view_.end()) {
            shown_ = *it;
            return false;
        }
        shown_.reset();
        typed_.clear();
    }
    openNext();
    const std::optional<std::uint64_t> after = shown_ ? std::optional<std::uint64_t>(shown_->id) : std::nullopt;
    return before != after;
}

void SecretPrompt::openNext() {
    const SecretRequest* oldest = nullptr;
    for (const SecretRequest& r : view_) {
        if (done_.count(r.id) == 0 && (oldest == nullptr || r.id < oldest->id)) {
            oldest = &r;
        }
    }
    if (oldest != nullptr) {
        shown_ = *oldest;
        typed_.clear();
    }
}

void SecretPrompt::lost() {
    view_.clear();
    shown_.reset();
    typed_.clear();
    done_.clear();
}

void SecretPrompt::type(std::string_view text) {
    if (shown_) {
        typed_ = SecretValue(text);
    }
}

bool SecretPrompt::submit(std::uint64_t* id, SecretValue* value) {
    if (!shown_) {
        return false;
    }
    const std::string& t = typed_.bytes();
    const char* blank = " \t\r\n";
    const std::size_t first = t.find_first_not_of(blank);
    if (first == std::string::npos) {
        return false;
    }
    const std::size_t last = t.find_last_not_of(blank);
    *value = SecretValue(std::string_view(t).substr(first, last - first + 1));
    *id = shown_->id;
    done_.insert(shown_->id);
    shown_.reset();
    typed_.clear();
    return true;
}

bool SecretPrompt::cancel(std::uint64_t* id) {
    if (!shown_) {
        return false;
    }
    *id = shown_->id;
    done_.insert(shown_->id);
    shown_.reset();
    typed_.clear();
    return true;
}

} // namespace nexus::sync
