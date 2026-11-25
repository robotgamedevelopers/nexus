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

std::string secretShape(const std::string& what, const SecretValue& value) {
    const std::string& b = value.bytes();
    if (b.empty()) {
        return "nothing was pasted";
    }
    if (b.size() > kSecretValueBytes) {
        return "what was pasted is longer than any key or token (at most 16 KiB): paste the " + what + " alone";
    }
    for (const char c : b) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) {
            return "what was pasted is not one token (it holds a space, a line break or a character that is not plain "
                "ASCII): paste the " + what + " alone";
        }
    }
    return std::string();
}

std::string SecretLog::line() const {
    return "secret request " + std::to_string(id) + " for " + account + ": " + event +
        (detail.empty() ? std::string() : ": " + detail);
}

std::vector<SecretRequest> SecretDesk::holding() const {
    std::vector<SecretRequest> out;
    for (const auto& [id, h] : held_) {
        out.push_back(h.request);
    }
    return out;
}

bool SecretDesk::busy(const std::string& account) const {
    return std::any_of(open_.begin(), open_.end(), [&](const SecretRequest& r) { return r.account == account; }) ||
        std::any_of(held_.begin(), held_.end(), [&](const auto& h) { return h.second.request.account == account; });
}

bool SecretDesk::waiting(SessionId asker) const {
    return std::any_of(open_.begin(), open_.end(), [&](const SecretRequest& r) { return r.asker == asker; }) ||
        std::any_of(held_.begin(), held_.end(), [&](const auto& h) { return h.second.request.asker == asker; });
}

SecretEffects SecretDesk::ask(std::uint64_t id, const SecretAsker& asker, const std::string& account,
                              const std::string& what, const std::string& why, bool takers, std::uint64_t now) {
    SecretEffects out;
    SecretRequest r{id, account, what, asker.session, asker.name, why};
    const bool isBusy = busy(account);
    if (isBusy || !takers) {
        const std::string why2 = isBusy ? reason::kBusy : reason::kNoClient;
        out.replies.push_back(SecretReply{id, asker, false, why2, account, what});
        out.notes.push_back(SecretNote{id, asker.session, "refused", account,
            note(r, isBusy ? "was not asked for: a request for it is open, or its value is being stored, already" :
                             "was not asked for: no window that can take it is connected")});
        out.log.push_back(SecretLog{id, account, why2, {}});
        remember(r, SecretState::Refused, asker);
        return out;
    }
    open_.push_back(r);
    askers_[id] = asker;
    due_[id] = now + kSecretDeadlineMs;
    out.notes.push_back(SecretNote{id, asker.session, "asked", account, note(r, "was asked for in nexus's window")});
    out.log.push_back(SecretLog{id, account, "asked", {}});
    out.changed = true;
    return out;
}

SecretEffects SecretDesk::refuseAtOnce(const SecretAsker& asker, const std::string& account, const std::string& what,
                                       const std::string& why) const {
    SecretEffects out;
    out.replies.push_back(SecretReply{0, asker, false, reason::kBusy, account, what});
    out.notes.push_back(SecretNote{0, asker.session, "refused", account,
        "secret ask: " + account + "'s " + what + " was not asked for: " + why});
    out.log.push_back(SecretLog{0, account, reason::kBusy, "not asked: session " + std::to_string(asker.session) +
                                ": " + why});
    return out;
}

void SecretDesk::end(std::size_t index, SecretState state, SecretEffects& out) {
    const SecretRequest r = open_[index];
    const SecretAsker asker = askers_[r.id];
    open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(index));
    askers_.erase(r.id);
    due_.erase(r.id);
    remember(r, state, asker);
    out.changed = true;
}

void SecretDesk::remember(const SecretRequest& r, SecretState state, const SecretAsker& asker) {
    ended_[r.id] = Ended{state, r, asker};
    while (ended_.size() > kKeptEnded) {
        ended_.erase(ended_.begin());
    }
}

// No request of `account` after `id` was open, held, stored or expired: each one asked since was refused (at its ask,
// or cancelled). Then an answer for `id` is still what the person meant the account's file to hold.
bool SecretDesk::newest(std::uint64_t id, const std::string& account) const {
    if (busy(account)) {
        return false;   // one open or held for the account is newer than any that ended
    }
    for (auto it = ended_.upper_bound(id); it != ended_.end(); ++it) {
        if (it->second.request.account == account && it->second.state != SecretState::Refused) {
            return false;
        }
    }
    return true;
}

// Writes `value` to `r`'s file: stored once durable; else held, to be written again at now + backoff.
void SecretDesk::write(const SecretRequest& r, const SecretAsker& asker, const SecretValue& value, bool answered,
                       bool first, SecretKeeper& keeper, std::uint64_t now, std::uint64_t backoff,
                       SecretEffects& out) {
    std::string why;
    const Kept kept = keeper.keep(r.account, r.what, value, &why);
    const bool captured = captured_.count(r.id) != 0;
    if (kept == Kept::Durable) {
        if (!answered) {
            out.replies.push_back(SecretReply{r.id, asker, true, std::string(), r.account, r.what});
        }
        out.notes.push_back(SecretNote{r.id, r.asker, "stored", r.account,
            note(r, captured ? "from the browser's sign-in was stored" :
                    answered ? "was stored after all: its answer crossed the deadline" : "was stored"), captured});
        captured_.erase(r.id);
        out.log.push_back(SecretLog{r.id, r.account, "stored", echoFault_ ? value.bytes() : std::string()});
        out.stored.push_back(r);
        remember(r, SecretState::Stored, asker);
        return;
    }
    const std::string said = kept == Kept::Unconfirmed ? why : "its file could not be written: " + why;
    held_[r.id] = Held{r, asker, value, answered, now + backoff, std::min(backoff * 2, kSecretRetryMaxMs)};
    out.log.push_back(SecretLog{r.id, r.account, "holding",
        said + "; the value is held in memory and written again in " + std::to_string(backoff / 1000) + " s"});
    if (first) {
        out.notes.push_back(SecretNote{r.id, r.asker, "holding", r.account,
            note(r, captured ?
                    "from the browser's sign-in is held: its file could not be written yet, so nexus writes it "
                    "again until it can (server.log says why); nothing needs signing in again" :
                    "arrived and is held: its file could not be written yet, so nexus writes it again until it can "
                    "(server.log says why); nothing needs pasting again"), captured});
    }
}

SecretEffects SecretDesk::capture(std::uint64_t id, const std::string& account, const std::string& what,
                                  const SecretValue& value, SecretKeeper& keeper, std::uint64_t now) {
    SecretEffects out;
    const SecretRequest r{id, account, what, 0, "nexus", "signed in in the browser"};
    if (const std::string problem = secretShape(what, value); !problem.empty()) {
        out.log.push_back(SecretLog{id, account, reason::kMalformed, "the sign-in's login: " + problem});
        remember(r, SecretState::Refused, SecretAsker{});
        return out;
    }
    captured_.insert(id);
    // Nobody waits for an answer: nexus started the sign-in itself, and the account's drivers start once it is stored.
    write(r, SecretAsker{}, value, true, true, keeper, now, kSecretRetryFirstMs, out);
    return out;
}

SecretEffects SecretDesk::answer(std::uint64_t id, const SecretValue& value, SecretKeeper& keeper,
                                 std::uint64_t now) {
    SecretEffects out;
    auto it = std::find_if(open_.begin(), open_.end(), [&](const SecretRequest& r) { return r.id == id; });
    SecretRequest r;
    SecretAsker asker;
    bool expired = false;
    if (it != open_.end()) {
        r = *it;
        asker = askers_[id];
    } else {
        // Answered in another window, cancelled, held already, or from before a restart: nothing is stored. Past its
        // deadline, and still the newest request of its account: the person pasted it for that file, so it is.
        auto e = ended_.find(id);
        if (e == ended_.end() || e->second.state != SecretState::Expired || !newest(id, e->second.request.account)) {
            out.log.push_back(SecretLog{id, std::string(), "late", {}});
            return out;
        }
        r = e->second.request;
        asker = e->second.asker;
        expired = true;
    }
    if (const std::string problem = secretShape(r.what, value); !problem.empty()) {
        // The person's input, not nexus's failure: they are told, and asked again (nexus's own ask is made again by
        // the hub; an AI's AI asks again itself).
        if (expired) {
            out.log.push_back(SecretLog{id, r.account, reason::kMalformed, problem + " (after its deadline)"});
            return out;
        }
        out.replies.push_back(SecretReply{id, asker, false, reason::kMalformed, r.account, r.what});
        out.notes.push_back(SecretNote{id, r.asker, "refused", r.account, note(r, "was not stored: " + problem)});
        out.log.push_back(SecretLog{id, r.account, reason::kMalformed, problem});
        end(static_cast<std::size_t>(it - open_.begin()), SecretState::Refused, out);
        return out;
    }
    if (expired) {
        ended_.erase(id);
    } else {
        open_.erase(it);
        askers_.erase(id);
        due_.erase(id);
        out.changed = true;
    }
    write(r, asker, value, expired, true, keeper, now, kSecretRetryFirstMs, out);
    return out;
}

SecretEffects SecretDesk::cancel(std::uint64_t id) {
    SecretEffects out;
    auto it = std::find_if(open_.begin(), open_.end(), [&](const SecretRequest& r) { return r.id == id; });
    if (it == open_.end()) {
        out.log.push_back(SecretLog{id, std::string(), "late", {}});
        return out;
    }
    const SecretRequest r = *it;
    out.replies.push_back(SecretReply{id, askers_[id], false, reason::kCancelled, r.account, r.what});
    out.notes.push_back(SecretNote{id, r.asker, "refused", r.account, note(r, "was not stored: the person cancelled")});
    out.log.push_back(SecretLog{id, r.account, reason::kCancelled, {}});
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
        out.replies.push_back(SecretReply{r.id, askers_[r.id], false, reason::kTimeout, r.account, r.what});
        out.notes.push_back(SecretNote{r.id, r.asker, "refused", r.account,
            note(r, "was not stored: nobody answered within 10 minutes")});
        out.log.push_back(SecretLog{r.id, r.account, reason::kTimeout, {}});
        end(i, SecretState::Expired, out);
    }
    return out;
}

SecretEffects SecretDesk::retry(std::uint64_t now, SecretKeeper& keeper) {
    SecretEffects out;
    for (auto it = held_.begin(); it != held_.end(); ++it) {
        if (it->second.at > now) {
            continue;
        }
        Held h = std::move(it->second);
        held_.erase(it);
        write(h.request, h.asker, h.value, h.answered, false, keeper, now, h.backoff, out);
        break;
    }
    return out;
}

SecretEffects SecretDesk::satisfied(const std::string& account, const std::string& what) {
    SecretEffects out;
    for (std::size_t i = 0; i < open_.size();) {
        const SecretRequest r = open_[i];
        if (r.account != account || r.what != what) {
            ++i;
            continue;
        }
        out.replies.push_back(SecretReply{r.id, askers_[r.id], true, std::string(), r.account, r.what});
        out.notes.push_back(SecretNote{r.id, r.asker, "stored", r.account,
            note(r, "is there already: the account was signed in in the browser, so nothing needs pasting")});
        out.log.push_back(SecretLog{r.id, r.account, "satisfied", "signed in in the browser"});
        end(i, SecretState::Stored, out);
    }
    return out;
}

SecretEffects SecretDesk::retireLogin(const std::string& account, const std::string& what) {
    SecretEffects out;
    auto matches = [&](const SecretRequest& r) { return r.account == account && r.what == what; };
    auto refuse = [&](const SecretRequest& r, const SecretAsker& asker, bool answered) {
        if (!answered) out.replies.push_back({r.id, asker, false, reason::kCancelled, account, what});
        out.log.push_back({r.id, account, reason::kCancelled, "this account now requires verified sign-in"});
    };
    for (std::size_t i = 0; i < open_.size();) {
        const SecretRequest r = open_[i];
        if (matches(r)) {
            refuse(r, askers_.at(r.id), false);
            end(i, SecretState::Refused, out);
        } else {
            ++i;
        }
    }
    for (auto it = held_.begin(); it != held_.end();) {
        const auto& h = it->second;
        if (matches(h.request)) {
            refuse(h.request, h.asker, h.answered);
            remember(h.request, SecretState::Refused, h.asker);
            captured_.erase(it->first);
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [id, e] : ended_) {
        if (e.state == SecretState::Expired && matches(e.request)) e.state = SecretState::Refused;
    }
    return out;
}

SecretEffects SecretDesk::drop() {
    SecretEffects out;
    while (!open_.empty()) {
        out.log.push_back(SecretLog{open_.front().id, open_.front().account, "dropped", {}});
        end(0, SecretState::Dropped, out);
    }
    while (!held_.empty()) {
        const Held& h = held_.begin()->second;
        out.log.push_back(SecretLog{h.request.id, h.request.account, "lost",
            captured_.count(h.request.id) != 0 ?
                "the server ended before its file could be written, so the login is gone and the account signs in "
                "again" :
                "the server ended before its file could be written, so the value is gone and must be pasted again"});
        remember(h.request, SecretState::Dropped, h.asker);
        held_.erase(held_.begin());
    }
    captured_.clear();
    // A server that starts again knows no request of this one: an answer for one that expired is late there.
    for (auto& [id, e] : ended_) {
        if (e.state == SecretState::Expired) {
            e.state = SecretState::Refused;
        }
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

std::optional<std::uint64_t> SecretDesk::retryAt() const {
    std::optional<std::uint64_t> at;
    for (const auto& [id, h] : held_) {
        if (!at || h.at < *at) {
            at = h.at;
        }
    }
    return at;
}

std::optional<SecretState> SecretDesk::stateOf(std::uint64_t id) const {
    if (isOpen(id)) {
        return SecretState::Open;
    }
    if (held_.count(id) != 0) {
        return SecretState::Holding;
    }
    auto it = ended_.find(id);
    if (it == ended_.end()) {
        return std::nullopt;
    }
    return it->second.state;
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
