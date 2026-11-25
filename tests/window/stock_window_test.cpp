// The window that a client with no applied policy bundle draws (a new install, the iPad) is the window the shipped
// profile draws on a desktop where it was applied: the stock Starlark client policy (policy/native defaults) runs with
// the shipped policy/rules/config.json. Checked through the window's own reading of the policy (window_app.h), for the
// tabs of a server with the sprint tab and two AIs: the tab titles are Dashboard and the AIs' names capitalised, and
// the account menu is centred, not in the tab. Built for the desktop (tests/window) and for iOS (tests/ios), where it
// runs on a simulator. Pure logic: no files, no clock, no network.

#include <cstdio>
#include <string>
#include <vector>

#include "policy/host.h"
#include "window_app.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

std::string joined(const std::vector<std::string>& titles) {
    std::string out;
    for (const auto& title : titles) out += (out.empty() ? "" : ", ") + title;
    return out;
}

} // namespace

int main() {
    nexus::policy::Host host("client");
    nexus::WindowApplication window;
    const std::string ui = R"({"tabs":[)"
        R"({"kind":"sprint","label":"sprint","group":"","type":""},)"
        R"({"kind":"friend","label":"rowan","group":"","type":""},)"
        R"({"kind":"friend","label":"stella","group":"","type":""}],)"
        R"("dismissed":false,"help_visible":false,"draft":"","logout":false})";
    window.update(host, "friend", "", true, false, 1, 3, "", {}, false, {}, ui);
    const std::vector<std::string> expected{"Dashboard", "Rowan", "Stella"};
    if (window.surface.tabTitles != expected)
        std::fprintf(stderr, "tab titles: [%s], expected [%s]\n", joined(window.surface.tabTitles).c_str(),
            joined(expected).c_str());
    CHECK(window.surface.tabTitles == expected);
    CHECK(window.centeredAccounts);
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("stock window: the shipped profile's tabs and account menu\n");
    return 0;
}
