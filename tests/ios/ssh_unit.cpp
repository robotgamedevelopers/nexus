// Unit tests of the in-app ssh (src/link/inapp_ssh.h): reading the link's ssh commands, the device key's public line
// (against one OpenSSH's ssh-keygen wrote), the known-hosts decision, the device key kept in a directory, and the
// runner's answers that need no connection. No sockets and no network: the one address resolved is numeric, which
// getaddrinfo answers without asking anyone. Run on a simulator device; its one argument is a directory of its own,
// under the build tree, that the test empties and uses.
//
// Usage: ios_ssh_unit DIR

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "link/bootstrap.h"
#include "link/inapp_ssh.h"

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

std::vector<std::uint8_t> fromHex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(std::uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

void reads_the_links_commands() {
    Bootstrap b;
    b.host = "me@mac";
    b.client = "window-0123456789abcdef";
    const Command run = bootstrapCommand(b);
    SshCall call;
    std::string why;
    CHECK(parseSshCall(run, &call, &why));
    CHECK(call.kind == SshCall::Kind::Run);
    CHECK(call.user == "me" && call.host == "mac");
    CHECK(call.line == run.arguments.back());
    CHECK(call.line.find("nexus-server") != std::string::npos);

    CHECK(parseSshCall(Command{"/usr/bin/ssh", {"-G", "me@mac"}, "ssh -G me@mac"}, &call, &why));
    CHECK(call.kind == SshCall::Kind::Resolve && call.user == "me" && call.host == "mac");
    CHECK(parseSshCall(Command{"/usr/bin/ssh", {"-G", "mac"}, "ssh -G mac"}, &call, &why));
    CHECK(call.user.empty() && call.host == "mac");

    // Anything else is refused, not guessed at.
    CHECK(!parseSshCall(Command{"/usr/bin/ssh", {"-L", "1:2:3", "mac"}, "ssh -L"}, &call, &why));
    CHECK(why.find("only the link's own commands") != std::string::npos);
    CHECK(!parseSshCall(Command{"/usr/bin/ssh", {"--", "ls"}, "ssh -- ls"}, &call, &why));
    CHECK(!parseSshCall(Command{"/usr/bin/ssh", {"mac", "--", "a", "b"}, "ssh mac -- a b"}, &call, &why));
    CHECK(!parseSshCall(Command{"/usr/bin/ssh", {"-G", "me@"}, "ssh -G me@"}, &call, &why));
}

void public_line_matches_openssh() {
    // The public half of a P-256 key ssh-keygen made (its private half was not kept), and the line it wrote.
    const auto point = fromHex("044cba1bdca843c885cfbce76895c6dc44b0658632763370f4a143f6249c703339dcb0d8eff342453adcbb"
                               "0d286778126a657c0257e89451b5e186ee7e3ea2d71a");
    CHECK(ecdsaPublicLine(point, "vector") ==
          "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBEy6G9yoQ8iFz7znaJXG3ESwZYYydjNw9KFD"
          "9iSccDM53LDY7/NCRTrcuw0oZ3gSamV8AlfolFG14Ybufj6i1xo= vector");
    auto compressed = point;
    compressed[0] = 0x02;
    CHECK(ecdsaPublicLine(compressed, "vector").empty());
    CHECK(ecdsaPublicLine(std::vector<std::uint8_t>(point.begin(), point.end() - 1), "vector").empty());
}

void known_hosts_trust_on_first_use() {
    const std::string line = knownHostLine("mac", "ecdsa-sha2-nistp256", "AAAAkey");
    CHECK(line == "mac ecdsa-sha2-nistp256 AAAAkey");
    CHECK(checkHostKey("", "mac", "ecdsa-sha2-nistp256", "AAAAkey") == HostKeyCheck::New);
    CHECK(checkHostKey(line + "\n", "mac", "ecdsa-sha2-nistp256", "AAAAkey") == HostKeyCheck::Known);
    CHECK(checkHostKey(line + "\n", "mac", "ecdsa-sha2-nistp256", "AAAAother") == HostKeyCheck::Changed);
    // Another host, or another type of key from the same host, is new, as OpenSSH has it.
    CHECK(checkHostKey(line + "\n", "space", "ecdsa-sha2-nistp256", "AAAAother") == HostKeyCheck::New);
    CHECK(checkHostKey(line + "\n", "mac", "ssh-rsa", "AAAAother") == HostKeyCheck::New);
    // Hosts named together, comments and blank lines.
    const std::string text = "# kept\n\nspace,mac ecdsa-sha2-nistp256 AAAAkey\n";
    CHECK(checkHostKey(text, "mac", "ecdsa-sha2-nistp256", "AAAAkey") == HostKeyCheck::Known);
    CHECK(checkHostKey(text, "mac", "ecdsa-sha2-nistp256", "AAAAnew") == HostKeyCheck::Changed);
    CHECK(checkHostKey(text, "stud", "ecdsa-sha2-nistp256", "AAAAnew") == HostKeyCheck::New);
}

void device_key_is_made_once_and_kept(const std::string& dir) {
    unlink((dir + "/ssh-key").c_str());
    unlink((dir + "/ssh-key.pub").c_str());
    DeviceKey first;
    DeviceKey again;
    std::string why;
    CHECK(deviceKey(dir, "nexus-ios", &first, &why));
    CHECK(first.privatePem.find("BEGIN EC PRIVATE KEY") != std::string::npos);
    CHECK(first.publicLine.rfind("ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABB", 0) == 0);
    CHECK(first.publicLine.size() > 20 && first.publicLine.substr(first.publicLine.size() - 10) == " nexus-ios");
    struct stat s{};
    CHECK(stat((dir + "/ssh-key").c_str(), &s) == 0 && (s.st_mode & 0777) == 0600);
    CHECK(deviceKey(dir, "nexus-ios", &again, &why));
    CHECK(again.privatePem == first.privatePem && again.publicLine == first.publicLine);

    // A key that does not read is said so, never replaced: the server may hold its public half.
    FILE* f = std::fopen((dir + "/ssh-key").c_str(), "w");
    if (f != nullptr) {
        std::fputs("not a key\n", f);
        std::fclose(f);
    }
    CHECK(!deviceKey(dir, "nexus-ios", &again, &why));
    CHECK(why.find("ssh-key") != std::string::npos);
    unlink((dir + "/ssh-key").c_str());
}

void runner_answers_without_a_connection(const std::string& dir) {
    InAppSshOptions o;
    o.dir = dir;
    const CommandRunner run = inAppSsh(o);
    Bootstrap b;
    b.host = "me@127.0.0.1";
    std::string ip;
    std::string why;
    CHECK(resolveHost(b, run, &ip, &why));
    CHECK(ip == "127.0.0.1");

    // A host with no user: the device has no ssh config to take one from.
    b.host = "127.0.0.1";
    const CommandResult r = run(bootstrapCommand(b), 5);
    CHECK(r.end == CommandResult::End::Exited && r.code == 255);
    CHECK(r.err.find("no user in `127.0.0.1`") != std::string::npos);
    Connect connect;
    CHECK(!bootstrap(b, run, &connect, &why));
    CHECK(why.find("no user") != std::string::npos);

    const CommandResult refused = run(Command{"/usr/bin/ssh", {"-N", "-L", "1:2:3", "x"}, "ssh -N"}, 5);
    CHECK(refused.end == CommandResult::End::NotStarted);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ios_ssh_unit DIR\n");
        return 2;
    }
    const std::string dir = argv[1];
    mkdir(dir.c_str(), 0700);
    reads_the_links_commands();
    public_line_matches_openssh();
    known_hosts_trust_on_first_use();
    device_key_is_made_once_and_kept(dir);
    runner_answers_without_a_connection(dir);
    if (failures != 0) {
        std::fprintf(stderr, "ios_ssh_unit: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ios_ssh_unit: all passed\n");
    return 0;
}
