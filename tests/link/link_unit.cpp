// Unit tests of the window's bootstrap (src/link/bootstrap.h): the CONNECT line, the line ssh runs, and the bootstrap
// and host lookup over a fake command runner. No processes, no sockets, no network (the one address resolved is
// numeric, which getaddrinfo answers without asking anyone).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "link/bootstrap.h"

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
    CHECK(c.port == "60001" && c.key == kKey);
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
    const std::vector<std::string> want = {"-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "studio", "--",
        remoteLine({"nexus-server", "new"})};
    CHECK(c.arguments == want);
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

struct Case {
    const char* name;
    void (*fn)();
};

const Case kCases[] = {
    {"connect_line_roundtrip", connect_line_roundtrip},
    {"connect_line_refusals", connect_line_refusals},
    {"remote_line_quotes_each_word", remote_line_quotes_each_word},
    {"bootstrap_command_shape", bootstrap_command_shape},
    {"bootstrap_reads_connect_line", bootstrap_reads_connect_line},
    {"bootstrap_failures_say_why_without_key", bootstrap_failures_say_why_without_key},
    {"resolve_host_uses_ssh_hostname", resolve_host_uses_ssh_hostname},
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
