// Unit tests of nexus-server's home and config (src/server/home.h) and of the settings files' parsing they share with
// the window (src/config.h): the home under HOME, no HOME, and every kind of line: a setting, a comment, a blank line,
// an unknown key (a warning) and a malformed or repeated one (an error naming the file and the line). Logic only: the
// files' text is a string, and HOME is an argument.

#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "server/home.h"

namespace {

using namespace nexus;
using namespace nexus::server;

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

void home() {
    std::string resolved;
    std::string top;
    std::string dir;
    std::string why;
    CHECK(serverHome("/home/me", &resolved, &top, &dir, &why));
    CHECK(resolved == "/home/me" && top == "/home/me/.nexus" && dir == "/home/me/.nexus/server");
    CHECK(serverHome("/home/me//", &resolved, &top, &dir, &why));
    CHECK(resolved == "/home/me" && top == "/home/me/.nexus" && dir == "/home/me/.nexus/server");
    CHECK(serverHome("/", &resolved, &top, &dir, &why));
    CHECK(resolved == "/" && top == "/.nexus" && dir == "/.nexus/server");
    CHECK(!serverHome(nullptr, &resolved, &top, &dir, &why) && has(why, "HOME is not set"));
    why.clear();
    CHECK(!serverHome("", &resolved, &top, &dir, &why) && has(why, "HOME is not set"));
}

void parsing() {
    const std::vector<std::string> keys{"nova-config", "sprint-url"};
    // Good lines, comments and blank lines; the value is the rest of the line, spaces inside kept.
    Config c = parseConfig("# settings\n\n  nova-config /opt/my tools/nova-config  \r\n\t# indented comment\n"
                           "sprint-url\thttp://127.0.0.1:8000/\n", "/h/.nexus/client/config", keys);
    CHECK(c.errors.empty() && c.warnings.empty());
    CHECK(c.values.size() == 2);
    CHECK(c.values.at("nova-config") == "/opt/my tools/nova-config");
    CHECK(c.values.at("sprint-url") == "http://127.0.0.1:8000/");
    // An unknown key: a warning naming the file, the line and the keys; the line is skipped.
    c = parseConfig("colour blue\nsprint-url http://h:1/\n", "/c", keys);
    CHECK(c.errors.empty() && c.warnings.size() == 1);
    CHECK(has(c.warnings[0], "/c line 1:") && has(c.warnings[0], "\"colour\"") && has(c.warnings[0], "`sprint-url`"));
    CHECK(c.values.size() == 1 && c.values.at("sprint-url") == "http://h:1/");
    // A key with no value, and a key given twice: errors naming the file and the line; the first value stands.
    c = parseConfig("sprint-url\nnova-config /a\nnova-config /b\n", "/c", keys);
    CHECK(c.errors.size() == 2 && c.warnings.empty());
    CHECK(has(c.errors[0], "/c line 1:") && has(c.errors[0], "no value"));
    CHECK(has(c.errors[1], "/c line 3:") && has(c.errors[1], "set again"));
    CHECK(c.values.size() == 1 && c.values.at("nova-config") == "/a");
    // No text: nothing set.
    c = parseConfig("", "/c", keys);
    CHECK(c.values.empty() && c.errors.empty() && c.warnings.empty());
}

void serverConfig() {
    std::vector<std::string> warnings;
    std::string why;
    // The server takes no settings yet: none is fine, an unknown key a warning, a malformed line an error.
    CHECK(readServerConfig("# nothing yet\n", "/s/config", &warnings, &why) && warnings.empty() && why.empty());
    CHECK(readServerConfig("port 9\n", "/s/config", &warnings, &why));
    CHECK(warnings.size() == 1 && has(warnings[0], "/s/config line 1:") && has(warnings[0], "takes no settings"));
    CHECK(!readServerConfig("\nport\n", "/s/config", &warnings, &why));   // malformed, whatever the key: an error
    CHECK(warnings.empty() && has(why, "/s/config line 2:") && has(why, "no value"));
    CHECK(!readServerConfig("port 1\nport 2\n", "/s/config", &warnings, &why));
    CHECK(warnings.size() == 1 && has(why, "/s/config line 2:") && has(why, "set again"));
}

} // namespace

int main() {
    home();
    parsing();
    serverConfig();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("home_test: all checks passed\n");
    return 0;
}
