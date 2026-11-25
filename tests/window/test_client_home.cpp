// Unit tests of the window's home and config (src/client_home.h): the home under HOME (USERPROFILE on Windows), no home,
// and the config's two keys with a comment, an unknown key and a malformed line. Logic only: the home and the file's
// text are arguments.

#include <cstdio>
#include <string>

#include "client_home.h"

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

#if defined(_WIN32)
constexpr const char* kHome = "C:\\Users\\me";
constexpr const char* kClient = "C:\\Users\\me\\.nexus\\client";
#else
constexpr const char* kHome = "/home/me";
constexpr const char* kClient = "/home/me/.nexus/client";
#endif

void home() {
    std::string dir;
    std::string why;
    CHECK(clientHomeFrom(kHome, &dir, &why) && dir == kClient);
    CHECK(clientHomeFrom((std::string(kHome) + "/").c_str(), &dir, &why) && dir == kClient);
    CHECK(!clientHomeFrom(nullptr, &dir, &why) && has(why, "is not set") && has(why, ".nexus/client"));
    why.clear();
    CHECK(!clientHomeFrom("", &dir, &why) && has(why, "is not set"));
}

void config() {
    // Both keys, with a comment and a blank line.
    ClientConfig c = clientConfigFrom("# mine\n\nnova-config /opt/nova/bin/nova-config\nserver studio\n",
        "/h/.nexus/client/config");
    CHECK(c.path == "/h/.nexus/client/config" && c.problems.empty());
    CHECK(c.novaConfig == "/opt/nova/bin/nova-config" && c.serverHost == "studio" && c.serverSet);
    // None: the defaults (nova-config from the PATH, the server on this machine).
    c = clientConfigFrom("", "/c");
    CHECK(c.novaConfig.empty() && c.problems.empty());
    CHECK(c.serverHost == "localhost" && !c.serverSet);
    // The dashboards are nexus-server's: the window's old dashboard settings are not settings, each a warning, and the
    // rest of the file is used.
    c = clientConfigFrom("sprint off\nsprint-url http://studio:8000/\nserver studio\n", "/c");
    CHECK(c.serverHost == "studio" && c.problems.size() == 2);
    CHECK(has(c.problems[0], "/c line 1:") && has(c.problems[0], "\"sprint\" is not a setting"));
    CHECK(has(c.problems[1], "/c line 2:") && has(c.problems[1], "\"sprint-url\" is not a setting"));
    // An unknown key is a warning and the rest is used; a malformed line is an error, and its key stays unset. Errors
    // come before warnings.
    c = clientConfigFrom("theme dark\nserver\nnova-config /n\n", "/c");
    CHECK(c.novaConfig == "/n" && c.serverHost == "localhost" && !c.serverSet);
    CHECK(c.problems.size() == 2);
    CHECK(has(c.problems[0], "/c line 2:") && has(c.problems[0], "`server` has no value"));
    CHECK(has(c.problems[1], "/c line 1:") && has(c.problems[1], "\"theme\" is not a setting"));
}

} // namespace

int main() {
    home();
    config();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_client_home: all checks passed\n");
    return 0;
}
