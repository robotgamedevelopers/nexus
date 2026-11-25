// Unit tests of the settings files' parsing (src/config.h), which the window (~/.nexus/client/config) and nexus-server
// (~/.nexus/server/config, tests/server/home_test.cpp) share: every kind of line: a setting, a comment, a blank line, an
// unknown key (a warning) and a malformed or repeated one (an error naming the file and the line). Logic only: the
// file's text is a string. It builds everywhere the window's code does, native Windows included.

#include <cstdio>
#include <string>
#include <vector>

#include "config.h"

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

void parsing() {
    const std::vector<std::string> keys{"nova-config", "server"};
    // Good lines, comments and blank lines; the value is the rest of the line, spaces inside kept.
    Config c = parseConfig("# settings\n\n  nova-config /opt/my tools/nova-config  \r\n\t# indented comment\n"
                           "server\thttp://127.0.0.1:8000/\n", "/h/.nexus/client/config", keys);
    CHECK(c.errors.empty() && c.warnings.empty());
    CHECK(c.values.size() == 2);
    CHECK(c.values.at("nova-config") == "/opt/my tools/nova-config");
    CHECK(c.values.at("server") == "http://127.0.0.1:8000/");
    // An unknown key: a warning naming the file, the line and the keys; the line is skipped.
    c = parseConfig("colour blue\nserver http://h:1/\n", "/c", keys);
    CHECK(c.errors.empty() && c.warnings.size() == 1);
    CHECK(has(c.warnings[0], "/c line 1:") && has(c.warnings[0], "\"colour\"") && has(c.warnings[0], "`server`"));
    CHECK(c.values.size() == 1 && c.values.at("server") == "http://h:1/");
    // A key with no value, and a key given twice: errors naming the file and the line; the first value stands.
    c = parseConfig("server\nnova-config /a\nnova-config /b\n", "/c", keys);
    CHECK(c.errors.size() == 2 && c.warnings.empty());
    CHECK(has(c.errors[0], "/c line 1:") && has(c.errors[0], "no value"));
    CHECK(has(c.errors[1], "/c line 3:") && has(c.errors[1], "set again"));
    CHECK(c.values.size() == 1 && c.values.at("nova-config") == "/a");
    // No text: nothing set.
    c = parseConfig("", "/c", keys);
    CHECK(c.values.empty() && c.errors.empty() && c.warnings.empty());
    // A list key: on as many lines as given, each kept in order with its line number; never "set again".
    c = parseConfig("item a\nnova-config /n\nitem b c\n\nitem a\n", "/c", keys, {"item"});
    CHECK(c.errors.empty() && c.warnings.empty() && c.values.size() == 1);
    CHECK((c.lists.at("item") == std::vector<std::pair<std::size_t, std::string>>{{1, "a"}, {3, "b c"}, {5, "a"}}));
    c = parseConfig("item\ncolour blue\n", "/c", keys, {"item"});
    CHECK(c.errors.size() == 1 && has(c.errors[0], "/c line 1:") && c.lists.empty());
    CHECK(c.warnings.size() == 1 && has(c.warnings[0], "`item` (on as many lines as you like)"));
}

} // namespace

int main() {
    parsing();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("config_test: all checks passed\n");
    return 0;
}
