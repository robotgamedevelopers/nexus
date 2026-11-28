// In-app sign-in's pure parts (sync/signin.h; models/SignIn.tla). Nothing here does I/O or reads a clock.

#include "sync/signin.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace nexus::sync {

const char* const kSignInIdsClient = "nexus:sign-ins";

namespace {

constexpr std::string_view kTokenPrefix = "sk-ant-";
// A subscription's login: an OAuth token, "sk-ant-oat01-" and token bytes, kSubscriptionTokenBytes in all.
constexpr std::string_view kSubscriptionPrefix = "sk-ant-oat01-";

bool isTokenByte(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
}

bool isSubscriptionShape(std::string_view b) {
    return b.size() == kSubscriptionTokenBytes && b.substr(0, kSubscriptionPrefix.size()) == kSubscriptionPrefix &&
        std::all_of(b.begin(), b.end(), isTokenByte);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexValue(s[i + 1]);
            const int lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string firstLabel(std::string_view host) {
    return lower(host.substr(0, host.find('.')));
}

const std::string kNotFound =
    "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 10\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\nnot found\n";

std::string answer(std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + std::string(body);
}

} // namespace

bool usesLocalSignInPage(std::string_view provider) {
    return provider == "codex" || provider == "antigravity" || provider == "grok" || provider == "muse";
}

bool hasSignIn(const std::string& provider) {
    return provider == "claude" || usesLocalSignInPage(provider);
}

std::string queryValue(std::string_view query, std::string_view name) {
    std::size_t at = 0;
    while (at <= query.size()) {
        std::size_t end = query.find('&', at);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        const std::string_view pair = query.substr(at, end - at);
        const std::size_t eq = pair.find('=');
        const std::string_view key = pair.substr(0, eq);
        if (key == name) {
            return eq == std::string_view::npos ? std::string() : percentDecode(pair.substr(eq + 1));
        }
        at = end + 1;
    }
    return std::string();
}

namespace {

// The hosts each harness's sign-in sends the browser to: the one list, which the server checks when the process hands
// its URL over and the window checks again before it opens a browser. claude: `claude setup-token`'s authorize pages
// (its CLAUDE_AI_AUTHORIZE_URL, claude.com; the console's, platform.claude.com; and claude.ai, which earlier versions
// of the program use). Compared byte for byte: a host in upper case, with a trailing dot, a port, userinfo or in
// punycode is not on it.
const std::vector<std::string_view>& signInHosts(std::string_view provider) {
    static const std::vector<std::string_view> kClaude{"claude.com", "platform.claude.com", "claude.ai"};
    static const std::vector<std::string_view> kNone;
    return provider == "claude" ? kClaude : kNone;
}

bool isLocalSignInUrl(std::string_view url, std::uint16_t* pagePort) {
    constexpr std::string_view origin = "http://127.0.0.1:";
    constexpr std::string_view route = "/nexus-signin/";
    if (!url.starts_with(origin)) {
        return false;
    }
    url.remove_prefix(origin.size());
    const std::size_t slash = url.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash > 5 || url.front() == '0') {
        return false;
    }
    unsigned port = 0;
    for (char c : url.substr(0, slash)) {
        if (c < '0' || c > '9') {
            return false;
        }
        port = port * 10 + static_cast<unsigned>(c - '0');
    }
    url.remove_prefix(slash);
    if (port < 1024 || port > 65535 || !url.starts_with(route)) {
        return false;
    }
    url.remove_prefix(route.size());
    *pagePort = static_cast<std::uint16_t>(port);
    return url.size() == 64 && std::all_of(url.begin(), url.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace

bool signInHostAllowed(std::string_view provider, std::string_view host) {
    const std::vector<std::string_view>& hosts = signInHosts(provider);
    return std::find(hosts.begin(), hosts.end(), host) != hosts.end();
}

std::string_view urlHost(std::string_view url) {
    if (url.substr(0, 8) != "https://") {
        return std::string_view();
    }
    const std::string_view rest = url.substr(8);
    return rest.substr(0, rest.find_first_of("/?#"));
}

bool sameBytes(std::string_view a, std::string_view b) {
    // Every byte of the longer is looked at whatever the others hold: the time says the lengths, never where they
    // differ.
    const std::size_t n = std::max(a.size(), b.size());
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0U;
        const unsigned y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0U;
        diff |= x ^ y;
    }
    return diff == 0;
}

bool isQueryBytes(std::string_view query) {
    return std::all_of(query.begin(), query.end(), [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u > 0x20 && u < 0x7f;
    });
}

bool parseSignInUrl(std::string_view provider, std::string_view url, SignInTarget* out) {
    if (usesLocalSignInPage(provider)) {
        std::uint16_t port = 0;
        if (url.size() > kSignInUrlBytes || !isLocalSignInUrl(url, &port)) {
            return false;
        }
        *out = {};
        out->port = port;
        return true;
    }
    if (url.size() > kSignInUrlBytes || url.substr(0, 8) != "https://" || !isQueryBytes(url)) {
        return false;
    }
    if (!signInHostAllowed(provider, urlHost(url))) {
        return false;
    }
    const std::size_t q = url.find('?');
    if (q == std::string_view::npos) {
        return false;
    }
    std::string_view query = url.substr(q + 1);
    query = query.substr(0, query.find('#'));
    const std::string redirect = queryValue(query, "redirect_uri");
    const std::string state = queryValue(query, "state");
    if (state.empty() || state.size() > 512) {
        return false;
    }
    std::string_view rest;
    std::string_view host;
    for (std::string_view h : {std::string_view("localhost"), std::string_view("127.0.0.1")}) {
        const std::string prefix = "http://" + std::string(h) + ":";
        if (std::string_view(redirect).substr(0, prefix.size()) == prefix) {
            rest = std::string_view(redirect).substr(prefix.size());
            host = h;
        }
    }
    std::size_t digits = 0;
    unsigned long port = 0;
    while (digits < rest.size() && digits < 6 && std::isdigit(static_cast<unsigned char>(rest[digits])) != 0) {
        port = port * 10 + static_cast<unsigned long>(rest[digits] - '0');
        ++digits;
    }
    if (digits == 0 || port == 0 || port > 65535) {
        return false;
    }
    std::string_view path = rest.substr(digits);
    if (path.empty()) {
        path = "/";
    }
    if (path[0] != '/' || !isQueryBytes(path) || path.find_first_of("?#") != std::string_view::npos) {
        return false;
    }
    out->port = static_cast<std::uint16_t>(port);
    out->path = std::string(path);
    out->state = state;
    out->host = std::string(host);
    return true;
}

bool isClaimToken(std::string_view token) {
    if (token.empty() || token.size() > kSignInTokenBytes) {
        return false;
    }
    return std::all_of(token.begin(), token.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

bool isSubscriptionToken(const SecretValue& token) {
    return isSubscriptionShape(token.bytes());
}

bool sameMachine(std::string_view serverArg, std::string_view host, std::string_view local) {
    std::string_view h = serverArg;
    const std::size_t at = h.rfind('@');
    if (at != std::string_view::npos) {
        h = h.substr(at + 1);
    }
    if (!h.empty() && h.front() == '[') {
        h = h.substr(1, h.find(']') == std::string_view::npos ? std::string_view::npos : h.find(']') - 1);
    } else if (std::count(h.begin(), h.end(), ':') == 1) {
        h = h.substr(0, h.find(':'));
    }
    const std::string name = lower(h);
    if (name == "localhost" || name == "::1" || name.substr(0, 4) == "127.") {
        return true;
    }
    const std::string a = firstLabel(host);
    const std::string b = firstLabel(local);
    return !a.empty() && a == b;
}

const char* signInStateName(SignInState state) {
    switch (state) {
        case SignInState::Needed: return "needed";
        case SignInState::Started: return "started";
        case SignInState::UrlSent: return "url-sent";
        case SignInState::Waiting: return "waiting";
        case SignInState::CallbackForwarded: return "callback-forwarded";
        case SignInState::Captured: return "captured";
        case SignInState::Failed: return "failed";
        case SignInState::Cancelled: return "cancelled";
        case SignInState::Expired: return "expired";
        case SignInState::Dropped: return "dropped";
        case SignInState::Cancelling: return "cancelling";
    }
    return "unknown";
}

bool signInRunning(SignInState state) {
    return state == SignInState::Started || state == SignInState::UrlSent || state == SignInState::Waiting ||
        state == SignInState::CallbackForwarded || state == SignInState::Cancelling;
}

// ---------------------------------------------------------------------------------------------------------------------
// SignInDesk

namespace {

std::string line(std::uint64_t id, const std::string& account, const std::string& what) {
    return "sign-in " + std::to_string(id) + ": " + account + ": " + what;
}

} // namespace

SignInDesk::Flow* SignInDesk::byId(std::uint64_t id) {
    for (auto& [account, f] : flows_) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

bool SignInDesk::live(const std::string& account) const {
    return flows_.count(account) != 0;
}

std::optional<std::pair<std::uint64_t, SignInState>> SignInDesk::flowOf(const std::string& account) const {
    const auto it = flows_.find(account);
    if (it == flows_.end()) {
        return std::nullopt;
    }
    return std::make_pair(it->second.id, it->second.state);
}

std::optional<SignInState> SignInDesk::stateOf(std::uint64_t id) const {
    for (const auto& [account, f] : flows_) {
        if (f.id == id) {
            return f.state;
        }
    }
    const auto it = ended_.find(id);
    if (it == ended_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SignInDesk::end(const std::string& account, SignInState state, SignInEffects& out) {
    const auto it = flows_.find(account);
    if (it == flows_.end()) {
        return;
    }
    Flow& f = it->second;
    if (signInRunning(f.state)) {
        out.stop.push_back(account);
    }
    f.url.clear();
    ended_[f.id] = state;
    while (ended_.size() > kKeptEnded) {
        ended_.erase(ended_.begin());
    }
    out.log.push_back(line(f.id, account, signInStateName(state)));
    switch (state) {
        case SignInState::Captured:
            out.notes.push_back({account, account + " was signed in in the browser"});
            break;
        case SignInState::Cancelled:
            out.notes.push_back({account, account + "'s sign-in was declined on the page: show nexus's window to "
                                 "sign in again"});
            break;
        case SignInState::Expired:
            out.notes.push_back({account, account + "'s sign-in was not finished in 10 minutes: show nexus's "
                                 "window to sign in again"});
            break;
        case SignInState::Failed:
            out.notes.push_back({account, account + "'s sign-in ended without a login: show nexus's window to "
                                 "sign in again"});
            break;
        default:
            break;
    }
    if (state != SignInState::Captured && state != SignInState::Dropped) {
        out.needAgain.push_back(account);
    }
    flows_.erase(it);
    out.changed = true;
}

SignInEffects SignInDesk::need(std::uint64_t id, const std::string& account, const std::string& provider) {
    SignInEffects out;
    if (live(account)) {
        return out;
    }
    flows_[account] = Flow{id, account, provider, SignInState::Needed, {}, {}, {}, 0, false};
    out.log.push_back(line(id, account, "needed"));
    out.notes.push_back({account, account + " needs signing in: show nexus's window (its hotkey) to sign in in the "
                         "browser"});
    out.changed = true;
    return out;
}

SignInEffects SignInDesk::claim(std::uint64_t id, const std::string& opener, std::uint64_t now) {
    SignInEffects out;
    Flow* f = byId(id);
    if (f == nullptr || !isClaimToken(opener)) {
        out.log.push_back("sign-in " + std::to_string(id) + ": a claim, late");
        return out;
    }
    if (f->state == SignInState::Needed) {
        f->state = SignInState::Started;
        f->opener = opener;
        f->due = now + kSignInDeadlineMs;
        out.start.push_back({f->id, f->account});
        out.log.push_back(line(f->id, f->account, "started"));
        out.changed = true;
    } else if (signInRunning(f->state) && f->opener.empty()) {
        f->opener = opener;
        if (claimFault_) {
            out.start.push_back({f->id, f->account});
        }
        out.log.push_back(line(f->id, f->account, "claimed"));
        out.changed = true;
    } else {
        out.log.push_back(line(f->id, f->account, "a claim, late"));
    }
    return out;
}

SignInEffects SignInDesk::url(const std::string& account, std::uint64_t id, const SecretValue& url) {
    SignInEffects out;
    const auto it = flows_.find(account);
    if (it == flows_.end() || it->second.id != id || it->second.state != SignInState::Started) {
        out.log.push_back("sign-in " + std::to_string(id) + ": " + account + ": a URL, late");
        return out;
    }
    Flow& f = it->second;
    SignInTarget target;
    if (!parseSignInUrl(f.provider, url.bytes(), &target)) {
        out.log.push_back(line(id, account, "the process's URL did not match the sign-in page for " + f.provider + ": refused"));
        end(account, SignInState::Failed, out);
        return out;
    }
    f.state = SignInState::UrlSent;
    f.url = url;
    f.target = std::move(target);
    out.log.push_back(line(id, account, "url"));
    out.changed = true;
    return out;
}

SignInEffects SignInDesk::opened(std::uint64_t id, const std::string& opener) {
    SignInEffects out;
    Flow* f = byId(id);
    if (f == nullptr || f->state != SignInState::UrlSent || f->opener != opener || opener.empty()) {
        out.log.push_back("sign-in " + std::to_string(id) + ": opened, late");
        return out;
    }
    f->state = SignInState::Waiting;
    out.log.push_back(line(id, f->account, "waiting"));
    out.changed = true;
    return out;
}

SignInEffects SignInDesk::callback(std::uint64_t id, const std::string& opener, const SecretValue& query) {
    SignInEffects out;
    Flow* f = byId(id);
    if (f == nullptr || usesLocalSignInPage(f->provider) || f->opener != opener || opener.empty() ||
            (f->state != SignInState::Waiting && !callbackFault_) || query.size() > kSignInQueryBytes) {
        out.log.push_back("sign-in " + std::to_string(id) + ": a callback, late");
        return out;
    }
    // Replayed as it came, in a request line: no space, CR, LF or other control byte may be in it.
    if (!isQueryBytes(query.bytes())) {
        out.log.push_back(line(id, f->account, "a callback with bytes no query holds, dropped"));
        return out;
    }
    const std::string state = queryValue(query.bytes(), "state");
    const bool code = !queryValue(query.bytes(), "code").empty();
    const bool error = !queryValue(query.bytes(), "error").empty();
    if (!callbackFault_ && (!sameBytes(state, f->target.state) || (!code && !error))) {
        out.log.push_back(line(id, f->account, "a callback whose state did not match, dropped"));
        return out;
    }
    f->state = SignInState::CallbackForwarded;
    f->denied = !code;
    out.replays.push_back({f->id, f->account, f->target, query});
    out.log.push_back(line(id, f->account, "forwarded"));
    out.changed = true;
    return out;
}

SignInEffects SignInDesk::cancel(std::uint64_t id, const std::string& opener) {
    SignInEffects out;
    auto* f = byId(id);
    if (!f || opener.empty() || f->opener != opener || !signInRunning(f->state) ||
            f->state == SignInState::CallbackForwarded || f->state == SignInState::Cancelling) return out;
    // Keep the account occupied until the runner releases its process and credential lock. Otherwise the next
    // sign-out races the stopped child and fails, or a replacement enrollment races its late credential write.
    f->state = SignInState::Cancelling;
    f->denied = true;
    f->url.clear();
    out.stop.push_back(f->account);
    out.changed = true;
    out.log.push_back(line(f->id, f->account, "cancelling"));
    return out;
}

std::uint64_t SignInDesk::printed(const std::string& account, SignInEffects* out) {
    const auto it = flows_.find(account);
    if (it == flows_.end() || !signInRunning(it->second.state)) {
        late(account, out);
        return 0;
    }
    const std::uint64_t id = it->second.id;
    end(account, SignInState::Captured, *out);
    return id;
}

void SignInDesk::late(const std::string& account, SignInEffects* out) {
    out->log.push_back(account + ": a sign-in's login, printed after its sign-in ended: kept");
    // The login is being stored: a needed flow for the account is not wanted any more.
    const auto it = flows_.find(account);
    if (it != flows_.end() && it->second.state == SignInState::Needed) {
        end(account, SignInState::Dropped, *out);
    }
}

SignInEffects SignInDesk::exited(const std::string& account, std::uint64_t id, const std::string& why) {
    SignInEffects out;
    const auto it = flows_.find(account);
    if (it == flows_.end() || it->second.id != id || !signInRunning(it->second.state)) {
        return out;
    }
    out.log.push_back(line(id, account, "its process ended: " + why));
    end(account, it->second.denied ? SignInState::Cancelled : SignInState::Failed, out);
    // Its process has ended already: nothing to stop.
    out.stop.erase(std::remove(out.stop.begin(), out.stop.end(), account), out.stop.end());
    return out;
}

SignInEffects SignInDesk::expire(std::uint64_t now) {
    SignInEffects out;
    std::vector<std::string> due;
    for (const auto& [account, f] : flows_) {
        if (signInRunning(f.state) && f.state != SignInState::Cancelling && dueAt(f) <= now) {
            due.push_back(account);
        }
    }
    for (const std::string& account : due) {
        end(account, SignInState::Expired, out);
    }
    return out;
}

SignInEffects SignInDesk::windowGone(const std::string& opener) {
    SignInEffects out;
    if (opener.empty()) {
        return out;
    }
    for (auto& [account, f] : flows_) {
        if (f.opener == opener) {
            f.opener.clear();
            out.log.push_back(line(f.id, f.account, "its window's link ended: let go of"));
            out.changed = true;
        }
    }
    return out;
}

SignInEffects SignInDesk::signedIn(const std::string& account) {
    SignInEffects out;
    end(account, SignInState::Dropped, out);
    return out;
}

std::vector<std::string> SignInDesk::fallBack(SignInEffects* out, const std::set<std::string>& browserOnly) {
    std::vector<std::string> accounts;
    for (const auto& [key, f] : flows_) {
        // A needed flow, and a running one that no window holds (its window went away): no window left could take it
        // over, so its process is ended rather than left to its deadline.
        if (!browserOnly.contains(key) && (f.state == SignInState::Needed || (signInRunning(f.state) && f.opener.empty()))) {
            accounts.push_back(key);
        }
    }
    for (const std::string& account : accounts) {
        out->log.push_back(line(flows_.at(account).id, account, "no window here signs in in the browser: a paste is "
                                "asked for instead"));
        end(account, SignInState::Dropped, *out);
    }
    return accounts;
}

SignInEffects SignInDesk::drop() {
    SignInEffects out;
    std::vector<std::string> all;
    for (const auto& [key, f] : flows_) {
        all.push_back(key);
    }
    for (const std::string& key : all) {
        end(key, SignInState::Dropped, out);
    }
    return out;
}

std::vector<SignInOffer> SignInDesk::offers() const {
    std::vector<SignInOffer> out;
    for (const auto& [account, f] : flows_) {
        SignInOffer o{f.id, f.account, f.provider, f.opener, host_, {}, true};
        if (!f.opener.empty() && signInRunning(f.state) && f.state != SignInState::Started) {
            o.url = f.url;
        }
        out.push_back(std::move(o));
    }
    std::sort(out.begin(), out.end(), [](const SignInOffer& a, const SignInOffer& b) { return a.id < b.id; });
    return out;
}

std::optional<std::uint64_t> SignInDesk::deadline() const {
    std::optional<std::uint64_t> out;
    for (const auto& [account, f] : flows_) {
        if (signInRunning(f.state) && f.state != SignInState::Cancelling && (!out || dueAt(f) < *out)) {
            out = dueAt(f);
        }
    }
    return out;
}

std::uint64_t SignInDesk::dueAt(const Flow& f) {
    // A flow whose callback went to its listener is exchanging the code for the login: it gets a short grace past its
    // deadline before its process is told to end, so that a login a moment from being printed is not cut off.
    return f.state == SignInState::CallbackForwarded ? f.due + kSignInExchangeGraceMs : f.due;
}

// ---------------------------------------------------------------------------------------------------------------------
// SignInOpener

void SignInOpener::received(const std::vector<SignInOffer>& offers) {
    view_ = offers;
    if (gate_ != 0) {
        const bool held = std::any_of(view_.begin(), view_.end(),
                                      [&](const SignInOffer& o) { return o.id == gate_ && o.opener == token_; });
        if (!held) {
            gate_ = 0;
        }
    }
}

void SignInOpener::lost() {
    view_.clear();
    gate_ = 0;
    armed_ = false;
}

std::uint64_t SignInOpener::cancelFor(const std::string& account, SecretValue* query) {
    for (const auto& offer : view_) {
        if (!gate_ || offer.id != gate_ || offer.account == account || offer.opener != token_) continue;
        SignInTarget target;
        if (!parseSignInUrl(offer.provider, offer.url.bytes(), &target) || target.state.empty()) return 0;
        std::string denial = "error=access_denied&state=";
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char c : target.state) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') denial += static_cast<char>(c);
            else { denial += '%'; denial += hex[c >> 4]; denial += hex[c & 15]; }
        }
        *query = SecretValue(denial);
        const auto id = gate_;
        gateClosed();
        return id;
    }
    return 0;
}

SignInOpener::Step SignInOpener::next(bool (*same)(const SignInOffer&, const void*), const void* context,
                                         const std::string* activeAccount) {
    Step step;
    if (token_.empty()) {
        return step;
    }
    for (const SignInOffer& o : view_) {
        if ((activeAccount && o.account != *activeAccount) || o.opener != token_ || o.url.empty() || opened_.count(o.id) != 0) {
            continue;
        }
        const bool gate = same == nullptr || !same(o, context);
        if (gate && usesLocalSignInPage(o.provider)) {
            continue;
        }
        // One gate per window: a page that needs one waits, unopened, while another flow's gate waits for its
        // callback (models/SignIn.tla Open, GateForEveryPage).
        if (gate && gate_ != 0) {
            continue;
        }
        opened_.insert(o.id);
        step.id = o.id;
        step.account = o.account;
        step.gate = gate;
        if (!parseSignInUrl(o.provider, o.url.bytes(), &step.target)) {
            // Not a sign-in page of the account's provider: nothing is opened, and the window says so; the flow ends
            // at its deadline.
            step.kind = Step::Kind::Refused;
            step.gate = false;
            step.target = SignInTarget{};
            return step;
        }
        step.kind = Step::Kind::Open;
        step.privatePage = usesLocalSignInPage(o.provider);
        step.url = o.url;
        if (step.gate) {
            gate_ = o.id;
        }
        return step;
    }
    if (!armed_ || activeAccount) {
        return step;
    }
    for (const SignInOffer& o : view_) {
        if (o.opener.empty() && claimed_.count(o.id) == 0 &&
            (wantedAccount_.empty() ? !usesLocalSignInPage(o.provider) : wantedAccount_ == o.account) &&
            (!usesLocalSignInPage(o.provider) || (same != nullptr && same(o, context)))) {
            claimed_.insert(o.id);
            armed_ = false;
            step.kind = Step::Kind::Claim;
            step.id = o.id;
            step.account = o.account;
            return step;
        }
    }
    return step;
}

// ---------------------------------------------------------------------------------------------------------------------
// CallbackGate

CallbackGate::Verdict CallbackGate::take(std::string_view bytes, SecretValue* query, std::string* response) {
    std::size_t end = bytes.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        end = bytes.find("\n\n");
    }
    if (end == std::string_view::npos || end > kRequestBytes) {
        if (bytes.size() < kRequestBytes) {
            return Verdict::More;
        }
        *response = kNotFound;
        return Verdict::Refuse;
    }
    const std::string_view first = bytes.substr(0, bytes.find_first_of("\r\n"));
    // GET <target> HTTP/1.x
    const std::size_t sp1 = first.find(' ');
    const std::size_t sp2 = first.rfind(' ');
    if (taken_ || sp1 == std::string_view::npos || sp2 == sp1 || first.substr(0, sp1) != "GET" ||
            first.substr(sp2 + 1, 7) != "HTTP/1.") {
        *response = kNotFound;
        return Verdict::Refuse;
    }
    const std::string_view target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = target.find('?');
    const std::string_view path = target.substr(0, q);
    const std::string_view rawQuery = q == std::string_view::npos ? std::string_view() : target.substr(q + 1);
    const bool code = !queryValue(rawQuery, "code").empty();
    const bool error = !queryValue(rawQuery, "error").empty();
    const bool match = path == target_.path && sameBytes(queryValue(rawQuery, "state"), target_.state) &&
        (code || error) && rawQuery.size() <= kSignInQueryBytes && isQueryBytes(rawQuery);
    if (!match && !strayFault_) {
        *response = kNotFound;
        return Verdict::Refuse;
    }
    taken_ = true;
    *query = SecretValue(rawQuery);
    *response = answer(code ? "Sign-in response received. Return to Nexus.\n" :
                              "sign-in was not finished, you can close this tab\n");
    return Verdict::Take;
}

// ---------------------------------------------------------------------------------------------------------------------
// GateSlots

bool GateSlots::accept(std::uint64_t now, std::size_t* slot) {
    for (std::size_t i = 0; i < kConnections; ++i) {
        if (!used_[i]) {
            used_[i] = true;
            due_[i] = now + kConnectionMs;
            *slot = i;
            return true;
        }
    }
    return false;
}

std::vector<std::size_t> GateSlots::expire(std::uint64_t now) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < kConnections; ++i) {
        if (used_[i] && due_[i] <= now) {
            used_[i] = false;
            out.push_back(i);
        }
    }
    return out;
}

void GateSlots::release(std::size_t slot) {
    if (slot < kConnections) {
        used_[slot] = false;
    }
}

std::size_t GateSlots::used() const {
    return static_cast<std::size_t>(std::count(std::begin(used_), std::end(used_), true));
}

// ---------------------------------------------------------------------------------------------------------------------
// TerminalText

namespace {

constexpr unsigned char kEsc = 0x1b;
constexpr unsigned char kCan = 0x18;
constexpr unsigned char kSub = 0x1a;
constexpr unsigned char kBel = 0x07;

} // namespace

TerminalText::Byte TerminalText::afterEscape(unsigned char u) {
    if (u == kEsc) {
        escape_ = 1;    // the first ESC is cut short; the second begins again
        return Byte::Break;
    }
    if (u == kCan || u == kSub) {
        escape_ = 0;
        return Byte::Break;
    }
    if (u == '[') {
        escape_ = 2;
        sgr_ = true;
        return Byte::Hidden;
    }
    if (u == ']' || u == 'P' || u == 'X' || u == '^' || u == '_') {
        escape_ = 3;
        return Byte::Hidden;
    }
    if (u >= 0x20 && u <= 0x2f) {
        escape_ = 5;    // an intermediate byte, as in a charset select (ESC ( B): its final byte follows
        return Byte::Hidden;
    }
    escape_ = 0;        // a two-byte escape (ESC 7, ESC =, ESC M) ends with this byte
    return Byte::Break;
}

TerminalText::Byte TerminalText::step(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (escape_) {
        case 1:
            return afterEscape(u);
        case 2:
            if (u == kCan || u == kSub) {
                escape_ = 0;
                return Byte::Break;
            }
            if (u == kEsc) {
                escape_ = 1;
                return Byte::Break;
            }
            if (u >= 0x30 && u <= 0x3f) {
                return Byte::Hidden;    // a parameter byte
            }
            if (u >= 0x20 && u <= 0x2f) {
                sgr_ = false;           // an intermediate byte: not SGR
                return Byte::Hidden;
            }
            if (u >= 0x40 && u <= 0x7e) {
                escape_ = 0;
                return u == 'm' && sgr_ ? Byte::Hidden : Byte::Break;
            }
            if (u < 0x20) {
                return Byte::Shown;     // a control inside a CSI is acted on, as terminals do
            }
            return Byte::Hidden;
        case 3:
            if (u == kBel || u == kCan || u == kSub) {
                escape_ = 0;
                return Byte::Break;
            }
            if (u == kEsc) {
                escape_ = 4;
            }
            return Byte::Hidden;
        case 4:
            if (u == '\\') {
                escape_ = 0;            // ST
                return Byte::Break;
            }
            // Any other byte after ESC ends the string, and the ESC begins a sequence of its own.
            (void)afterEscape(u);
            return Byte::Break;
        case 5:
            if (u == kCan || u == kSub) {
                escape_ = 0;
                return Byte::Break;
            }
            if (u == kEsc) {
                escape_ = 1;
                return Byte::Break;
            }
            if (u >= 0x20 && u <= 0x2f) {
                return Byte::Hidden;
            }
            escape_ = 0;
            return Byte::Break;
        default:
            break;
    }
    if (u == kEsc) {
        escape_ = 1;
        return Byte::Hidden;
    }
    return Byte::Shown;
}

// ---------------------------------------------------------------------------------------------------------------------
// UrlScanner

namespace {

constexpr std::string_view kUrlStart = "https://";

bool isUrlByte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u > 0x20 && u < 0x7f;
}

} // namespace

UrlScanner::UrlScanner(std::string provider) : provider_(std::move(provider)) {}

UrlScanner::~UrlScanner() {
    runBytes_ = sizeof run_;
    wipeRun();
}

void UrlScanner::wipeRun() {
    volatile char* p = run_;
    for (std::size_t i = 0; i < runBytes_; ++i) {
        p[i] = 0;
    }
    runBytes_ = 0;
}

SecretValue UrlScanner::take() {
    SecretValue out = std::move(url_);
    url_.clear();
    return out;
}

void UrlScanner::endRun() {
    if (runBytes_ > kUrlStart.size() && std::string_view(run_, kUrlStart.size()) == kUrlStart && line_.empty()) {
        const std::string_view candidate(run_, runBytes_);
        SignInTarget t;
        if (parseSignInUrl(provider_, candidate, &t)) {
            line_ = SecretValue(candidate);
        }
    }
    wipeRun();
}

void UrlScanner::endLine() {
    if (!found_ && !line_.empty()) {
        url_ = std::move(line_);
        found_ = true;
    }
    line_.clear();
}

bool UrlScanner::feed(std::string_view bytes) {
    for (char c : bytes) {
        if (found_) {
            break;
        }
        // Every escape sequence ends a URL's run, a colour change (SGR) too: a URL is never coloured part way along, and
        // the words a colour change leads into are not the URL's (a token's run, below, goes on across an SGR).
        if (term_.step(c) != TerminalText::Byte::Shown) {
            endRun();
            continue;
        }
        if (cr_) {
            if (c == '\n') {
                cr_ = false;
                endLine();
                continue;
            }
            if (c == '\r') {
                continue;
            }
            cr_ = false;
            line_.clear();      // the line starts over: what was on it is not text
        }
        if (c == '\r') {
            endRun();
            cr_ = true;
            continue;
        }
        if (c == '\n') {
            endRun();
            endLine();
            continue;
        }
        if (!isUrlByte(c)) {
            endRun();
            continue;
        }
        if (runBytes_ == kSignInUrlBytes) {
            wipeRun();
            if (c == 'h') {
                run_[runBytes_++] = c;
            }
            continue;
        }
        run_[runBytes_++] = c;
        // Only what may start a URL is kept: a run of URL bytes is dropped as soon as it cannot be one.
        const std::size_t n = std::min(runBytes_, kUrlStart.size());
        if (std::string_view(run_, n) != kUrlStart.substr(0, n)) {
            wipeRun();
            if (c == 'h') {
                run_[runBytes_++] = c;  // it may start one
            }
        }
    }
    return found_;
}

// ---------------------------------------------------------------------------------------------------------------------
// TokenScanner

TokenScanner::~TokenScanner() {
    runBytes_ = sizeof run_;
    wipeRun();
}

void TokenScanner::wipeRun() {
    volatile char* p = run_;
    for (std::size_t i = 0; i < runBytes_; ++i) {
        p[i] = 0;
    }
    runBytes_ = 0;
    runTotal_ = 0;
    inToken_ = false;
}

SecretValue TokenScanner::take() {
    SecretValue out = std::move(token_);
    token_.clear();
    return out;
}

void TokenScanner::push(char c) {
    if (!inToken_) {
        // Before the prefix: its last bytes alone, which may be the prefix's start.
        if (runBytes_ == kTokenPrefix.size()) {
            std::memmove(run_, run_ + 1, runBytes_ - 1);
            --runBytes_;
        }
        run_[runBytes_++] = c;
        if (runBytes_ == kTokenPrefix.size() && std::string_view(run_, runBytes_) == kTokenPrefix) {
            inToken_ = true;
            runTotal_ = runBytes_;
        }
        return;
    }
    ++runTotal_;
    if (runBytes_ < sizeof run_) {
        run_[runBytes_++] = c;
    }
}

void TokenScanner::endRun() {
    if (inToken_) {
        const std::string_view run(run_, runBytes_);
        if (runTotal_ == kSubscriptionTokenBytes && isSubscriptionShape(run)) {
            line_ = SecretValue(run);
        } else {
            passedOver_ = runTotal_;
        }
    }
    wipeRun();
}

void TokenScanner::endLine() {
    if (!line_.empty()) {
        last_ = std::move(line_);
        ++candidates_;
    }
    line_.clear();
}

void TokenScanner::feed(std::string_view bytes) {
    if (found_) {
        return;
    }
    for (char c : bytes) {
        const TerminalText::Byte b = term_.step(c);
        if (b == TerminalText::Byte::Hidden) {
            continue;
        }
        if (b == TerminalText::Byte::Break) {
            endRun();
            continue;
        }
        if (cr_) {
            if (c == '\n') {
                cr_ = false;
                endLine();
                continue;
            }
            if (c == '\r') {
                continue;
            }
            cr_ = false;
            line_.clear();      // the line starts over: what was on it is not text
        }
        if (c == '\r') {
            endRun();
            cr_ = true;
            continue;
        }
        if (c == '\n') {
            endRun();
            endLine();
            continue;
        }
        if (isTokenByte(c)) {
            push(c);
        } else {
            endRun();
        }
    }
}

bool TokenScanner::finish() {
    if (!found_) {
        endRun();
        endLine();      // the line under way stands: nothing came after it
        if (!last_.empty()) {
            token_ = std::move(last_);
            found_ = true;
        }
        last_.clear();
    }
    return found_;
}

} // namespace nexus::sync

