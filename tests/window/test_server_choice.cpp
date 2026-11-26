// Unit tests of the server a window asks for (src/server_choice.h): the address's checks, server.txt's text, and the
// kept server read back through a store in memory. Pure logic: no files, no clock, no network.

#include <cstdio>
#include <string>

#include "server_choice.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// A good address is taken as typed, spaces at its ends dropped; a device that needs the user refuses a bare host.
void addressesTaken() {
    std::string address;
    std::string why;
    CHECK(parseServerAddress("me@mac.example.ts.net", true, &address, &why) && address == "me@mac.example.ts.net");
    CHECK(parseServerAddress("  me@mac \t", true, &address, &why) && address == "me@mac");
    CHECK(parseServerAddress("me.you_2-x@10.0.0.7", true, &address, &why) && address == "me.you_2-x@10.0.0.7");
    CHECK(parseServerAddress("me@[fd7a:115c::1]", true, &address, &why) && address == "me@[fd7a:115c::1]");
    CHECK(parseServerAddress("mac", false, &address, &why) && address == "mac");
    CHECK(!parseServerAddress("mac", true, &address, &why) && has(why, "user@host"));
}

// Everything else is refused, with a reason a person can act on, and `address` is left alone.
void addressesRefused() {
    std::string address = "kept";
    std::string why;
    CHECK(!parseServerAddress("", true, &address, &why) && has(why, "type the server"));
    CHECK(!parseServerAddress("   ", true, &address, &why) && has(why, "type the server"));
    CHECK(!parseServerAddress("me@mac\nrm", true, &address, &why) && has(why, "control"));
    CHECK(!parseServerAddress(std::string("me@m\0c", 6), true, &address, &why) && has(why, "control"));
    CHECK(!parseServerAddress("me@mac\x7f", true, &address, &why) && has(why, "control"));
    CHECK(!parseServerAddress("me@my mac", true, &address, &why) && has(why, "no spaces"));
    CHECK(!parseServerAddress("@mac", true, &address, &why) && has(why, "no user"));
    CHECK(!parseServerAddress("me@", true, &address, &why) && has(why, "no host"));
    CHECK(!parseServerAddress("me@-oProxyCommand=x", true, &address, &why) && has(why, "cannot start with -"));
    CHECK(!parseServerAddress("me@.mac", true, &address, &why) && has(why, "cannot start with"));
    CHECK(!parseServerAddress("me@ma;c", true, &address, &why) && has(why, "host takes"));
    CHECK(!parseServerAddress("me@a@mac", true, &address, &why) && has(why, "host takes"));
    CHECK(!parseServerAddress("m$e@mac", true, &address, &why) && has(why, "user takes"));
    CHECK(!parseServerAddress("me@[mac", true, &address, &why) && has(why, "IPv6"));
    CHECK(!parseServerAddress("me@[::1;]", true, &address, &why) && has(why, "IPv6"));
    CHECK(!parseServerAddress("me@" + std::string(kServerAddressMax, 'a'), true, &address, &why) && has(why, "longer"));
    CHECK(address == "kept");
}

// server.txt: its first line, then one address on one line; anything else is refused.
void fileFormat() {
    CHECK(formatServerFile("me@mac") == "nexus server 1\nme@mac\n");
    std::string address;
    std::string why;
    CHECK(parseServerFile("nexus server 1\nme@mac\n", true, &address, &why) && address == "me@mac");
    CHECK(!parseServerFile("me@mac\n", true, &address, &why) && has(why, "first line"));
    CHECK(!parseServerFile("nexus server 2\nme@mac\n", true, &address, &why) && has(why, "first line"));
    CHECK(!parseServerFile("nexus server 1\nme@mac", true, &address, &why) && has(why, "one line"));
    CHECK(!parseServerFile("nexus server 1\nme@mac\nyou@pc\n", true, &address, &why) && has(why, "one line"));
    CHECK(!parseServerFile("nexus server 1\n\n", true, &address, &why) && has(why, "type the server"));
    CHECK(!parseServerFile("nexus server 1\nmac\n", true, &address, &why) && has(why, "user@host"));
}

struct Memory {
    bool saved = false;
    std::string text;
    std::string readError;
    TabOrderStore store() {
        return TabOrderStore{"/fake/server.txt",
            [this](std::string* t, std::string* e) {
                *e = readError;
                if (!saved || !readError.empty()) return false;
                *t = text;
                return true;
            },
            [this](const std::string& t, std::string*) {
                saved = true;
                text = t;
                return true;
            }};
    }
};

// The kept server: none kept is empty with no warning (the window asks); a kept one comes back; a bad or unreadable
// file is empty with a warning naming it (the window asks again).
void keptServer() {
    Memory mem;
    std::string warning;
    CHECK(loadServerAddress(mem.store(), true, &warning).empty() && warning.empty());
    std::string error;
    CHECK(mem.store().write(formatServerFile("me@mac"), &error));
    CHECK(loadServerAddress(mem.store(), true, &warning) == "me@mac" && warning.empty());
    mem.text = "nexus server 1\nme@-x\n";
    CHECK(loadServerAddress(mem.store(), true, &warning).empty() && has(warning, "/fake/server.txt") &&
        has(warning, "asks again"));
    mem.readError = "it cannot be read";
    CHECK(loadServerAddress(mem.store(), true, &warning).empty() && has(warning, "cannot be read"));
    const TabOrderStore none = fileServerStore("");
    CHECK(!none.write("x", &error) && has(error, "no home"));
}

} // namespace

int main() {
    addressesTaken();
    addressesRefused();
    fileFormat();
    keptServer();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("server choice: all checks passed\n");
    return 0;
}
