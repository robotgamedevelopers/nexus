// How long an account's stored login lasts, and what nexus-server does about its end (docs/accounts.md, "How long a
// login lasts"). The owner's rule (2026-11-14): a person pastes a key or signs in once, ever; nexus never loses it and
// never asks again, except for a login the provider revoked or that expired with no way to renew it, and then the
// dialog says which.
//
// Per provider: a login that can be renewed without a person is renewed by the server before its end, with no prompt
// (none of nexus's providers stores one yet: the rows for codex and gemini are on the roadmap). One that cannot is the
// one honest exception: the server knows when it ends, says so in plain words well before (30, 7 and 1 days ahead, in
// every session on the account and in server.log), and on the day asks for a new one in the window, saying why.
//
// A claude account's login is the token `claude setup-token` makes (the file oauth-token): one year, by Anthropic's
// documentation, with no refresh token and no way to renew it but a person running `claude setup-token` again. Its end
// is counted from when its file was written (the file's modification time): a token made before it was pasted ends
// that much sooner, which the 30 days' notice covers.
//
// inspectLogin and hasEnrollment inspect local files. Time is an argument to the lifetime checks.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sync/hub.h"

namespace nexus::server {

constexpr std::uint64_t kDayMs = 24ULL * 60 * 60 * 1000;

// What a provider's stored login is and how long it lasts. lifetimeMs 0: no known end (an API key lasts until it is
// revoked). renewable: the server can renew it without a person.
struct LoginTerm {
    std::uint64_t lifetimeMs = 0;
    bool renewable = false;
    std::string name;           // the login, in words: "Claude login"
    std::string makeNew;        // how a person makes a new one, in words for the dialog
};
LoginTerm loginTerm(const std::string& provider);

// A login file as it lies: missing; usable (a regular file of this user's, mode 0600 or narrower, holding one token);
// not of a token's shape (a paste can fix it); or unusable as it lies (a link, another user's, open to others,
// unreadable: a paste cannot fix that, the file must be). Never the value.
struct LoginSeen {
    enum class File { Missing, Usable, Malformed, Unusable } file = File::Missing;
    std::uint64_t storedAtMs = 0;   // the file's modification time, when usable
    std::string problem;            // for Malformed and Unusable, with the fix
};
// POSIX: nexus-server runs on macOS and Linux, and inside WSL2 on Windows, whose native build is only the window.
#ifndef _WIN32
// Open the requested account directory only with its exact entry spelling. Caller closes the descriptor.
int openAccountDirectory(const std::string& dir, std::string& problem);
// Read the token from its exact account directory. Wipe the value after its shape is checked.
LoginSeen inspectLogin(const std::string& path);
// Checks the named harness account's enrollment receipt under HOME. The receipt holds only the provider name.
bool hasEnrollment(const std::string& home, const std::string& provider, const std::string& account);
#endif

// The day `ms` falls on, UTC, as YYYY-MM-DD.
std::string dateOf(std::uint64_t ms);

// Whether a paste can fix `account`'s lost login (its driver reported no credential), and the words: ask in the window
// for the first login, for one that ended, for one not of a token's shape, or for one the provider refused (revoked,
// or no longer valid); for a file unusable as it lies, ask nothing and tell the sessions the fix.
sync::Hub::LoginAsk loginAsk(const std::string& account, const std::string& provider, const LoginSeen& seen,
                             std::uint64_t now);

// Whether `account`'s login is signed in at the server's start: usable, and not past its end.
bool loginLive(const std::string& provider, const LoginSeen& seen, std::uint64_t now);

// Watches the ends of the stored logins. Each notice goes once per login (a login stored again starts over), in order:
// 30 days before its end, 7, 1, and on the day itself (expired). A restart forgets what was told, so the notice due
// then is told once more.
class LoginWatch {
public:
    struct Login {
        std::string account;
        std::string provider;
        std::uint64_t storedAtMs = 0;
    };
    struct Notice {
        std::string account;
        bool expired = false;       // true: the end has come (ask for a new one, with `text` as the dialog's reason)
        std::string text;
    };
    std::vector<Notice> check(const std::vector<Login>& logins, std::uint64_t now);

private:
    struct Told {
        std::uint64_t storedAtMs = 0;
        int stage = 0;              // 0 nothing yet, 1 the 30 days' notice, 2 the 7 days', 3 the day's before, 4 ended
    };
    std::map<std::string, Told> told_;
};

} // namespace nexus::server
