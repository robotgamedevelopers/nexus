// The phone list's own rows (menu_rows on the window answer, docs/policy.md The iPhone's form), through the window's
// own reader (src/window_app.h readMenuRows): each a title and a window command that takes no argument, run as its
// shortcut runs it; absent is none; anything malformed is refused with why. Pure: no window, no OS, no clock.

#include "window_app.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using nexus::MenuRow;
using nexus::WindowCommand;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

bool read(const std::string& json, std::vector<MenuRow>& rows, std::string& why) {
    why.clear();
    return nexus::readMenuRows(json, rows, why);
}

void absentIsNone() {
    std::vector<MenuRow> rows{MenuRow{"stale", {}}};
    std::string why;
    CHECK(read(R"({"panels":["menu"]})", rows, why));
    CHECK(rows.empty());
}

void rowsAreReadInOrder() {
    std::vector<MenuRow> rows;
    std::string why;
    CHECK(read(R"({"menu_rows":[{"title":"New terminal","command":"open_terminal"},)"
               R"({"command":"stop","title":"Stop"}]})", rows, why));
    CHECK(rows.size() == 2);
    if (rows.size() == 2) {
        CHECK(rows[0].title == "New terminal");
        CHECK(rows[0].command.command == WindowCommand::OpenTerminal);
        CHECK(rows[1].title == "Stop");
        CHECK(rows[1].command.command == WindowCommand::Stop);
    }
}

void everyPlainCommand() {
    const std::pair<const char*, WindowCommand> commands[] = {
        {"open_terminal", WindowCommand::OpenTerminal}, {"close_terminal", WindowCommand::CloseTerminal},
        {"close_tab", WindowCommand::CloseTab},         {"invoke_service", WindowCommand::InvokeService},
        {"clear_composer", WindowCommand::ClearComposer}, {"stop", WindowCommand::Stop}};
    for (const auto& [name, command] : commands) {
        std::vector<MenuRow> rows;
        std::string why;
        CHECK(read(std::string(R"({"menu_rows":[{"title":"x","command":")") + name + R"("}]})", rows, why));
        CHECK(rows.size() == 1 && rows[0].command.command == command);
    }
}

void malformedIsRefusedLoudly() {
    const char* bad[] = {
        R"({"menu_rows":{"title":"x","command":"stop"}})",                       // not a list
        R"({"menu_rows":["stop"]})",                                              // a row not an object
        R"({"menu_rows":[{"title":"x"}]})",                                       // no command
        R"({"menu_rows":[{"command":"stop"}]})",                                  // no title
        R"({"menu_rows":[{"title":"","command":"stop"}]})",                       // empty title
        R"({"menu_rows":[{"title":"a\nb","command":"stop"}]})",                   // two lines
        R"({"menu_rows":[{"title":"x","command":"rm_rf"}]})",                     // no such command
        R"({"menu_rows":[{"title":"x","command":"select_tab"}]})",                // takes an argument
        R"({"menu_rows":[{"title":"x","command":"stop","extra":1}]})",            // something else
        R"({"menu_rows":[{"title":1,"command":"stop"}]})",                        // title not text
    };
    for (const char* json : bad) {
        std::vector<MenuRow> rows;
        std::string why;
        const bool ok = read(json, rows, why);
        CHECK(!ok);
        CHECK(!why.empty());
        if (ok) std::fprintf(stderr, "  accepted: %s\n", json);
    }
    std::string nine = R"({"menu_rows":[)";
    for (int i = 0; i < 9; ++i) nine += std::string(i ? "," : "") + R"({"title":"x","command":"stop"})";
    nine += "]}";
    std::vector<MenuRow> rows;
    std::string why;
    CHECK(!read(nine, rows, why)); // at most 8
    CHECK(why.find("at most 8") != std::string::npos);
    std::string title129(129, 'a');
    CHECK(!read(R"({"menu_rows":[{"title":")" + title129 + R"(","command":"stop"}]})", rows, why));
    CHECK(read(R"({"menu_rows":[{"title":")" + title129.substr(1) + R"(","command":"stop"}]})", rows, why));
}

} // namespace

int main() {
    absentIsNone();
    rowsAreReadInOrder();
    everyPlainCommand();
    malformedIsRefusedLoudly();
    if (failures) {
        std::fprintf(stderr, "%d menu-rows check(s) failed\n", failures);
        return 1;
    }
    std::printf("menu_rows: all checks passed\n");
    return 0;
}
