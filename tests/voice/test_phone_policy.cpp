// The iPhone's form as the shipped client policy composes it (policy/rules/client/window.star, docs/voice.md), through
// the window's own reader (src/window_app.h WindowApplication): the list of tabs and its order, an AI's tab with the
// way back, its transcript and its prompt, how replies come back in each, and the iPad and desktop unchanged. Pure
// logic over the packaged Starlark: no window, no OS, no clock.

#include "window_app.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using nexus::Panel;
using nexus::WindowApplication;

int failures = 0;
nexus::policy::Host* policyHost = nullptr;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

// The strip's order: a terminal, two AIs, the dashboard. The list puts the dashboard first, then the AIs, then terminals.
const std::string kTabs = R"("tabs":[{"kind":"terminal","label":"zsh","group":"terminals","type":""},)"
                          R"({"kind":"friend","label":"emma","group":"friends","type":""},)"
                          R"({"kind":"friend","label":"rowan","group":"friends","type":""},)"
                          R"({"kind":"sprint","label":"dashboard","group":"sprint","type":""}])";

std::string ui(const char* idiom, bool menu) {
    return std::string("{") + kTabs + R"(,"idiom":")" + idiom + R"(","menu":)" + (menu ? "true" : "false") +
           R"(,"voice":{"listening":false,"speaking":false},"draft":""})";
}

WindowApplication window(const std::string& context, const char* tab = "friend") {
    // The host lives in main: one destroyed after main returns tears down the policy runtime out of order.
    auto& host = *policyHost;
    WindowApplication app;
    app.update(host, tab, "", true, false, 1, 4, "friends", {}, false, {}, context);
    return app;
}

void thePhoneOpensOnTheList() {
    const auto app = window(ui("phone", true));
    CHECK((app.panels == std::vector<Panel>{Panel::Menu}));
    CHECK(!app.surface.showTabs);
    CHECK((app.surface.tabTitles == std::vector<std::string>{"zsh", "Emma", "Rowan", "dashboard"}));
    // The dashboard, the AIs, then terminals.
    CHECK(app.surface.menuTabs.has_value());
    CHECK((app.surface.menuTabs.value_or(std::vector<std::size_t>{}) == std::vector<std::size_t>{3, 1, 2, 0}));
    CHECK(app.surface.voiceMic);
}

void anAIsTabIsTheTranscriptAndThePrompt() {
    const auto app = window(ui("phone", false));
    CHECK((app.panels == std::vector<Panel>{Panel::Back, Panel::Transcript, Panel::Account, Panel::Prompt}));
    CHECK(!app.surface.showTabs);
    CHECK(!app.surface.focusPrompt); // the keyboard waits for a tap on the prompt
    CHECK(app.surface.voiceMic);     // the prompt's dictation
}

void theListWithNoTabs() {
    const auto app = window(R"({"tabs":[],"idiom":"phone","menu":true,"draft":""})", "empty");
    CHECK((app.panels == std::vector<Panel>{Panel::Menu}));
}

void theIPadAndTheDesktopKeepTheStrip() {
    for (const char* idiom : {"pad", "desktop"}) {
        const auto app = window(ui(idiom, true));
        CHECK(app.surface.showTabs);
        CHECK(app.surface.focusPrompt);
        CHECK(!app.surface.voiceMic);
        CHECK(!app.surface.menuTabs.has_value());
        CHECK((app.panels == std::vector<Panel>{Panel::Transcript, Panel::Account, Panel::Prompt}));
    }
}

} // namespace

int main() {
    nexus::policy::Host host("client");
    policyHost = &host;
    thePhoneOpensOnTheList();
    anAIsTabIsTheTranscriptAndThePrompt();
    theListWithNoTabs();
    theIPadAndTheDesktopKeepTheStrip();
    if (failures) {
        std::fprintf(stderr, "%d phone-policy check(s) failed\n", failures);
        return 1;
    }
    std::printf("phone_policy: all checks passed\n");
    return 0;
}
