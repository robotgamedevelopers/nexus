// In-app sign-in's processes on the server (docs/secrets.md "Sign-in"; the flow's rules are sync/signin.h's desk, in
// the hub). For each flow a window claims, the runner starts the unmodified `claude setup-token` for its account:
//
// - under a pseudo-terminal of its own, very wide, so the program draws as it does for a person and prints each line
//   whole;
// - with an environment made from nothing but HOME, PATH (nexus's helper first), TERM, LANG, its own CLAUDE_CONFIG_DIR
//   under the account's directory (made 0700, removed when the process ends), BROWSER (the helper) and the helper's
//   two variables. No API key variable, nor anything else of the server's environment, reaches it;
// - with nexus's helper (nexus-signin-open) first on its PATH as `open`, `xdg-open`, `wslview`, `sensible-browser`,
//   `www-browser` and `x-www-browser`, and as BROWSER: it hands the URL to the runner over a Unix socket and opens
//   nothing. Where the program opens a browser by a path of its own instead, the URL it then prints in its terminal is
//   taken (sync::UrlScanner).
//
// The process's terminal output goes only through the scanners, which keep a few bytes at a time and wipe them: the
// URL once (to the hub, then to the claiming window alone), the token once the program has ended (the last login it
// printed, straight to the one-chance store), its terminal read to the end first. Nothing else of the output is kept,
// logged or shown. A window's callback is replayed once to the program's listener on the server's loopback (the
// redirect's own host: localhost is tried on ::1, then on 127.0.0.1, as browsers do; 127.0.0.1 on that alone), its
// answer read and dropped. The runner never writes to `claude setup-token`'s input. The program runs in a process group of its
// own, and the group is what is ended: by stop(), at its grace, and once the program has ended, so that nothing it
// started outlives it.
//
// Other providers use their configured enrollment helper (drivers/rpc nexus_rpc.enroll). Its authenticated socket
// messages carry its URL (a local page's, or for a provider that signs in by a pasted code, the vendor's own sign-in
// URL), then for the latter `?code` once its vendor's CLI asks for the code. Terminal output is discarded. A code a
// window pasted (SignInDesk::code took it) is written once to the helper's terminal, with echo off there, as one
// line: the one thing the runner ever writes to a program's input (models/SignInPaste.tla CodeWith). A successful exit
// asks the hub to verify the named enrollment receipt.
//
// POSIX (a pty, poll, posix_spawn), like the rest of nexus-server: macOS and Linux, and Linux inside WSL2 on Windows.

#pragma once

#include <poll.h>
#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sync/secrets.h"
#include "sync/signin.h"

namespace nexus::server {

std::string claudeOnPath(const std::string& path);

class SignInRunner {
public:
    struct Options {
        std::string claude;         // the claude program; empty: the first `claude` on PATH
        std::string helper;         // nexus-signin-open, beside nexus-server
        std::string accounts;       // the accounts' directory (server/home.h accountsDir)
        std::string dir;            // the server's directory: the helper's socket and its bin directory go in it
        std::string home;           // HOME for the program
        std::string path;           // the PATH the program's own PATH ends with (the server's)
        // A test seam, set only by tests/server/signin_functional's own runner (nexus-server never sets it): replay
        // here, not to the URL's port.
        std::uint16_t replayPort = 0;
        std::uint64_t (*now)() = nullptr;
    };
    struct Events {
        std::function<void(const std::string& account, std::uint64_t id, const sync::SecretValue& url)> url;
        // The helper of flow `id` says its vendor's CLI waits for the code (after its URL, once).
        std::function<void(const std::string& account, std::uint64_t id)> codeWanted;
        std::function<void(const std::string& account, const sync::SecretValue& token)> printed;
        std::function<void(const std::string& account, std::uint64_t id, const std::string& why)> exited;
        std::function<void(const std::string& line)> log;
        std::function<void(const std::string& account, const std::string& provider, std::uint64_t id)> enrolled;
    };

    SignInRunner(Options options, Events events);
    ~SignInRunner();
    SignInRunner(const SignInRunner&) = delete;
    SignInRunner& operator=(const SignInRunner&) = delete;

    // The helper's socket and bin directory. False, with why, when either cannot be made. First, what a server that
    // ended without its runner's own end (a crash, a SIGKILL) left: sweep().
    bool open(std::string* why);
    // For each account's directory under the accounts' directory, a sign-in's config directory (.signin) left behind:
    // its `claude setup-token`, if it still runs, is ended (its process group: SIGTERM, then SIGKILL after
    // kKillGraceMs), and the directory removed. A process is taken as this server's own only while it holds the lock
    // in .signin/lock that the runner gave it at its start (an inherited descriptor); its group is .signin/pid. One
    // line each, for server.log.
    std::vector<std::string> sweep();
    // The hub's hooks.
    void start(std::uint64_t id, const std::string& account, const std::string& provider = "claude",
               std::vector<std::string> command = {});
    void stop(const std::string& account);
    void replay(std::uint64_t id, const std::string& account, const sync::SignInTarget& target,
                const sync::SecretValue& query);
    // A pasted code for flow `id` of `account` (the hub's code hook): written once to that helper's terminal, a line,
    // when the helper still runs for that flow and asked; dropped otherwise (logged without its bytes).
    void code(std::uint64_t id, const std::string& account, const sync::SecretValue& code);

    // The fds the server's poll waits on, and what came in on them (and any process past its grace). Calls the events;
    // true when it called any.
    std::vector<pollfd> fds() const;
    bool serve();
    // How long the server's poll may wait for the runner: a process being ended, or a replay under way, wants a look
    // within 100 ms; else -1 (only its fds).
    int waitMs() const;
    bool running(const std::string& account) const { return procs_.count(account) != 0; }

private:
    struct Proc {
        std::uint64_t id = 0;
        pid_t pid = -1;
        int master = -1;
        std::string nonce;          // the helper's word that it was started for this flow
        std::string configDir;
        int lock = -1;              // .signin/lock, locked; the process holds it too (its fd 3), for sweep()
        sync::TokenScanner token;
        sync::UrlScanner url{"claude"};     // the runner runs `claude setup-token`: its hosts
        std::string provider;
        bool setupToken = false;
        bool urlSent = false;
        bool codeAsked = false;     // its helper said `?code`
        bool codeTaken = false;     // a code was written, or is being: no other
        std::string codeOut;        // what of the code's line is still to write; wiped as it goes
        std::uint64_t killAt = 0;   // when SIGKILL follows the SIGTERM; 0: not ending
    };
    struct Helper {
        int fd = -1;
        std::string bytes;
        std::uint64_t until = 0;
    };
    struct Replay {
        int fd = -1;
        std::string out;            // the request still to write; wiped as it goes
        std::uint64_t until = 0;
        bool sent = false;          // a byte of it went: the connection is made
        std::uint16_t port = 0;
        std::vector<int> families;  // the address families still to try, after the one `fd` is for
        std::uint64_t id = 0;
        std::string account;
    };
    void readTerminal(const std::string& account, Proc& p);
    // The process ended (its status, already collected) or is ended now: its terminal read to the end, its group
    // ended, its directory removed, and its login handed over or its end reported (unless the flow ended first:
    // `reported`).
    void reap(const std::string& account, Proc& p, bool reported, std::optional<int> status);
    bool connectReplay(Replay& r);
    void writeCode(Proc& p);
    void acceptHelper();
    void readHelper(Helper& h);
    std::uint64_t now() const;
    Options options_;
    Events events_;             // as given, each wrapped to mark `acted_`
    bool acted_ = false;
    std::string socket_;
    std::string bin_;
    int listen_ = -1;
    std::map<std::string, Proc> procs_;
    std::vector<Helper> helpers_;
    std::vector<Replay> replays_;
};

} // namespace nexus::server
