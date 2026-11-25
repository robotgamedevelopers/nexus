// The iPhone's form as the shipped client policy composes it (policy/rules/client/window.star, docs/voice.md), through
// the window's own reader (src/window_app.h WindowApplication): the menu of tabs, one tab with the way back, the
// microphone and the prompt, voice-only replies, the fork's voice settings, and the iPad and desktop unchanged. Pure
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

const std::string kTabs = R"("tabs":[{"kind":"friend","label":"emma","group":"friends","type":""},)"
                          R"({"kind":"friend","label":"rowan","group":"friends","type":""}])";

std::string ui(const char* idiom, bool menu, const char* reply = "both") {
    return std::string("{") + kTabs + R"(,"idiom":")" + idiom + R"(","menu":)" + (menu ? "true" : "false") +
           R"(,"voice":{"listening":false,"speaking":false,"reply":")" + reply + R"("},"draft":""})";
}

WindowApplication window(const std::string& context, const char* tab = "friend") {
    // The host lives in main: one destroyed after main returns tears down the policy runtime out of order.
    auto& host = *policyHost;
    WindowApplication app;
    app.update(host, tab, "", true, false, 0, 2, "friends", {}, false, {}, context);
    return app;
}

void thePhoneOpensOnTheMenu() {
    const auto app = window(ui("phone", true));
    CHECK((app.panels == std::vector<Panel>{Panel::Menu}));
    CHECK(!app.surface.focusPrompt); // the keyboard waits for a tap on the prompt
    CHECK(!app.surface.showTabs);
    CHECK((app.surface.tabTitles == std::vector<std::string>{"Emma", "Rowan"}));
}

void aTabOpensFullScreenWithTheMicrophone() {
    const auto app = window(ui("phone", false));
    CHECK((app.panels == std::vector<Panel>{Panel::Back, Panel::Transcript, Panel::Account, Panel::Voice, Panel::Prompt}));
    CHECK(!app.surface.showTabs);
    CHECK(app.surface.voiceReply == "both");
}

void voiceAloneGivesTheTranscriptsRoomToTheMicrophone() {
    const auto app = window(ui("phone", false, "voice"));
    CHECK((app.panels == std::vector<Panel>{Panel::Back, Panel::Spacer, Panel::Account, Panel::Voice, Panel::Prompt}));
}

void theMenuWithNoTabs() {
    const auto app = window(R"({"tabs":[],"idiom":"phone","menu":true,"draft":""})", "empty");
    CHECK((app.panels == std::vector<Panel>{Panel::Menu}));
}

void theIPadAndTheDesktopKeepTheStrip() {
    for (const char* idiom : {"pad", "desktop"}) {
        const auto app = window(ui(idiom, true));
        CHECK(app.surface.showTabs);
        CHECK(app.surface.focusPrompt);
        CHECK((app.panels == std::vector<Panel>{Panel::Transcript, Panel::Account, Panel::Prompt}));
    }
}

} // namespace

int main() {
    nexus::policy::Host host("client");
    policyHost = &host;
    thePhoneOpensOnTheMenu();
    aTabOpensFullScreenWithTheMicrophone();
    voiceAloneGivesTheTranscriptsRoomToTheMicrophone();
    theMenuWithNoTabs();
    theIPadAndTheDesktopKeepTheStrip();
    if (failures) {
        std::fprintf(stderr, "%d phone-policy check(s) failed\n", failures);
        return 1;
    }
    std::printf("phone_policy: all checks passed\n");
    return 0;
}
