// The dashboards a server offers its windows (docs/formats.md, SessionsDiff.dashboards): pages the person chose to keep
// beside their AIs, each a tab at the head of every window's strip, labelled with its name. nexus-server takes them from
// its own config, one `dashboard <url> <name>` line each, in order (server/home.h); none is the default. Pure.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::sync {

struct Dashboard {
    std::string name;   // the tab's label, and what a saved layout and selection know it by
    std::string url;    // the page, http:// or https://

    bool operator==(const Dashboard&) const = default;
};

// The bounds a server keeps and a window holds it to: what a tab strip can show and a diff carries whole.
constexpr std::size_t kDashboardsMax = 16;
constexpr std::size_t kDashboardNameMax = 64;
constexpr std::size_t kDashboardUrlMax = 2048;

// Why `d` is not a dashboard a window can show, or empty when it is: a name of 1 to kDashboardNameMax bytes of UTF-8
// with no control character, and an http:// or https:// address with a host, of at most kDashboardUrlMax bytes, with
// no space or control character.
std::string dashboardProblem(const Dashboard& d);

// One config line's value, `<url> <name>` (the name is the rest of the line and may hold spaces), into `out`. False,
// with why, when it is malformed or dashboardProblem refuses it.
bool parseDashboardLine(std::string_view value, Dashboard* out, std::string* why);

// Why `list` is not a list a server sends, or empty when it is: at most kDashboardsMax, each one shown
// (dashboardProblem), and no name twice.
std::string dashboardsProblem(const std::vector<Dashboard>& list);

} // namespace nexus::sync
