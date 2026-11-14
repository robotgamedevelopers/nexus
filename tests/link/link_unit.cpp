// Unit tests of the window's bootstrap (src/link/bootstrap.h): the CONNECT line, the request nexus-server new makes of
// the running server (src/server/server.h), the line ssh runs, and the bootstrap
// and host lookup over a fake command runner. No processes, no sockets, no network (the one address resolved is
// numeric, which getaddrinfo answers without asking anyone).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "link/bootstrap.h"
#include "server/server.h"
#include "sync/formats.h"

namespace {

using namespace nexus;
using namespace nexus::link;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

// A made-up key of the right shape: 22 base64 characters.
const char kKey[] = "AbCdEfGhIjKlMnOpQrSt+/";

void connect_line_roundtrip() {
    Connect c;
    CHECK(parseConnect(connectLine("60001", kKey), &c));
    CHECK(c.port == "60001" && c.key == kKey && c.format == 1);    // a line that states no format: format 1
    CHECK(connectLine("60001", kKey) == "NEXUS CONNECT 60001 " + std::string(kKey));   // as before formats
    CHECK(parseConnect(connectLine("60001", kKey, 2), &c));
    CHECK(c.port == "60001" && c.key == kKey && c.format == 2);
    CHECK(connectLine("60001", kKey, 2) == "NEXUS CONNECT 60001 " + std::string(kKey) + " format 2");
}

// What a window that states a format asks of nexus-server new, and what the server makes of each request: a window
// before formats states none, and is answered as before.
void new_request_states_the_format() {
    server::Client c;
    CHECK(server::newRequest(c) == "new");
    c.id = "window-1";
    c.next = 12;
    CHECK(server::newRequest(c) == "new window-1 12");
    c.format = 2;
    CHECK(server::newRequest(c) == "new window-1 12 format 2");
    server::Client read;
    CHECK(server::parseNewRequest("new window-1 12 format 2", &read));
    CHECK(read.id == "window-1" && read.next == 12 && read.format == 2);
    CHECK(server::parseNewRequest("new format 3", &read));
    CHECK(read.id.empty() && read.next == 1 && read.format == 3);
    CHECK(server::parseNewRequest("new", &read) && read.format == 0);
    for (const char* bad : {"new format", "new format 0", "new format x", "new window-1", "new window-1 0",
            "new window-1 12 format", "new window-1 12 13", "news", "new  format 2", ""}) {
        CHECK(!server::parseNewRequest(bad, &read));
    }
    // No format is refused today; a window stating none is served format 1.
    server::Client none;
    CHECK(server::formatRefusalReply(none).empty());
    none.format = 1;
    CHECK(server::formatRefusalReply(none).empty());
}

void connect_line_refusals() {
    const std::vector<std::string> bad = {
        "", "NEXUS CONNECT", "NEXUS CONNECT 60001", "MOSH CONNECT 60001 " + std::string(kKey),
        "NEXUS CONNECT 0 " + std::string(kKey), "NEXUS CONNECT 070 " + std::string(kKey),
        "NEXUS CONNECT 65536 " + std::string(kKey), "NEXUS CONNECT 6000a " + std::string(kKey),
        "NEXUS CONNECT 60001 " + std::string(kKey) + "A", "NEXUS CONNECT 60001 " + std::string(kKey).substr(1),
        "NEXUS CONNECT 60001  " + std::string(kKey), "NEXUS CONNECT 60001 AbCdEfGhIjKlMnOpQrSt-_",
        " NEXUS CONNECT 60001 " + std::string(kKey),
    };
    for (const std::string& line : bad) {
        Connect c{"keep", "keep"};
        CHECK(!parseConnect(line, &c));
        CHECK(c.port == "keep");
    }
    CHECK(parseConnect("NEXUS CONNECT 65535 " + std::string(kKey), nullptr));
}

void remote_line_quotes_each_word() {
    CHECK(remoteLine({"nexus-server", "new"}) == "exec \"$SHELL\" -l -c ''\\''nexus-server'\\'' '\\''new'\\'''");
    // a quote inside a word: closed, escaped and reopened at each of the two levels
    CHECK(remoteLine({"it's"}) == "exec \"$SHELL\" -l -c ''\\''it'\\''\\'\\'''\\''s'\\'''");
}

void bootstrap_command_shape() {
    Bootstrap b;
    b.host = "studio";
    const Command c = bootstrapCommand(b);
    CHECK(c.program == "/usr/bin/ssh");
    // The window states the state format it reads in nexus-server new's environment, which a server before formats
    // ignores.
    const std::vector<std::string> want = {"-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "studio", "--",
        remoteLine({"env", "NEXUS_STATE_FORMAT=" + std::to_string(sync::kStateFormat), "nexus-server", "new"})};
    CHECK(c.arguments == want);
    b.stateFormat = 0;      // states none, as a window before formats
    CHECK(bootstrapCommand(b).arguments.back() == remoteLine({"nexus-server", "new"}));
}

// kLocalHost: no ssh; the placed server under HOME, run directly, with the state format in its environment.
void bootstrap_command_local() {
    Bootstrap b;
    b.host = kLocalHost;
    b.home = "/h";
    CHECK(isLocal(b));
    Command c = bootstrapCommand(b);
    CHECK(c.program == "/usr/bin/env");
    const std::vector<std::string> want = {"NEXUS_STATE_FORMAT=" + std::to_string(sync::kStateFormat),
        "/h/.nexus/bin/nexus-server", "new"};
    CHECK(c.arguments == want);
    b.stateFormat = 0;
    c = bootstrapCommand(b);
    CHECK((c.arguments == std::vector<std::string>{"/h/.nexus/bin/nexus-server", "new"}));
    // Not placed: the reason says how to place it.
    Connect out;
    std::string why;
    CHECK(!bootstrap(b, [](const Command&, int) {
        CommandResult r;
        r.end = CommandResult::End::Exited;
        r.code = 127;
        r.err = "env: /h/.nexus/bin/nexus-server: No such file or directory";
        return r;
    }, &out, &why));
    CHECK(why.find("nexus-server setup") != std::string::npos);
    b.host = "studio";
    CHECK(!isLocal(b));
}

CommandResult result(CommandResult::End end, int code, std::string out, std::string err) {
    CommandResult r;
    r.end = end;
    r.code = code;
    r.out = std::move(out);
    r.err = std::move(err);
    return r;
}

void bootstrap_reads_connect_line() {
    Bootstrap b;
    b.host = "studio";
    int calls = 0;
    auto run = [&](const Command&, int timeout) {
        ++calls;
        CHECK(timeout == 30);
        return result(CommandResult::End::Exited, 0, "motd\r\nNEXUS CONNECT 60007 " + std::string(kKey) + "\r\n", "");
    };
    Connect c;
    std::string why;
    CHECK(bootstrap(b, run, &c, &why));
    CHECK(c.port == "60007" && c.key == kKey && calls == 1);
}

void bootstrap_failures_say_why_without_key() {
    Bootstrap b;
    b.host = "studio";
    Connect c;
    std::string why;
    auto notFound = [](const Command&, int) {
        return result(CommandResult::End::Exited, 127, "", "zsh:1: command not found: nexus-server\n");
    };
    CHECK(!bootstrap(b, notFound, &c, &why));
    CHECK(why.find("command not found: nexus-server") != std::string::npos);
    CHECK(why.find("~/.local/bin") != std::string::npos);
    auto refused = [](const Command&, int) {
        return result(CommandResult::End::Exited, 255, "", "ssh: connect to host studio port 22: Connection refused\n");
    };
    CHECK(!bootstrap(b, refused, &c, &why));
    CHECK(why.find("Connection refused") != std::string::npos && why.find("password") != std::string::npos);
    // a CONNECT line that does not parse may hold a key: it is never quoted
    auto malformed = [](const Command&, int) {
        return result(CommandResult::End::Exited, 0, "NEXUS CONNECT 60001 " + std::string(kKey) + "XYZ\n", "");
    };
    CHECK(!bootstrap(b, malformed, &c, &why));
    CHECK(why.find(kKey) == std::string::npos);
    CHECK(why.find("does not parse") != std::string::npos);
    auto timeout = [](const Command&, int) { return result(CommandResult::End::TimedOut, 0, "", ""); };
    CHECK(!bootstrap(b, timeout, &c, &why));
    CHECK(why.find("did not finish in time") != std::string::npos);
}

void resolve_host_uses_ssh_hostname() {
    Bootstrap b;
    b.host = "studio";
    std::vector<std::string> args;
    auto run = [&](const Command& c, int) {
        args = c.arguments;
        return result(CommandResult::End::Exited, 0, "user someone\nhostname 127.0.0.1\nport 22\n", "");
    };
    std::string ip;
    std::string why;
    CHECK(resolveHost(b, run, &ip, &why));
    CHECK(ip == "127.0.0.1");
    CHECK((args == std::vector<std::string>{"-G", "studio"}));
    auto broken = [](const Command&, int) { return result(CommandResult::End::Exited, 255, "", "bad config\n"); };
    CHECK(!resolveHost(b, broken, &ip, &why));
    CHECK(why.find("bad config") != std::string::npos);
}

// The server's paths: --dir moves its own files, never the logins, which stay under HOME where the drivers read them
// (the sign-in check reads paths.accounts).
void server_logins_stay_under_home() {
    ::setenv("HOME", "/h/", 1);
    server::Paths p;
    std::string why;
    CHECK(server::paths("", &p, &why));
    CHECK(p.dir == "/h/.nexus/server" && p.accounts == "/h/.nexus/server/accounts");
    CHECK(server::paths("/t/other", &p, &why));
    CHECK(p.dir == "/t/other" && p.drivers == "/t/other/drivers" && p.accounts == "/h/.nexus/server/accounts");
}

struct Case {
    const char* name;
    void (*fn)();
};

const Case kCases[] = {
    {"connect_line_roundtrip", connect_line_roundtrip},
    {"new_request_states_the_format", new_request_states_the_format},
    {"connect_line_refusals", connect_line_refusals},
    {"remote_line_quotes_each_word", remote_line_quotes_each_word},
    {"bootstrap_command_shape", bootstrap_command_shape},
    {"bootstrap_command_local", bootstrap_command_local},
    {"bootstrap_reads_connect_line", bootstrap_reads_connect_line},
    {"bootstrap_failures_say_why_without_key", bootstrap_failures_say_why_without_key},
    {"resolve_host_uses_ssh_hostname", resolve_host_uses_ssh_hostname},
    {"server_logins_stay_under_home", server_logins_stay_under_home},
};

} // namespace

int main(int argc, char** argv) {
    std::size_t ran = 0;
    for (const Case& c : kCases) {
        if (argc == 2 && std::strcmp(argv[1], c.name) != 0)
            continue;
        const int before = failures;
        c.fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", c.name);
        ++ran;
    }
    if (ran == 0) {
        std::fprintf(stderr, "link_unit: no case named %s\n", argc == 2 ? argv[1] : "");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
