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

// Why `name` is not a dashboard's name, or empty when it is: 1 to kDashboardNameMax bytes of UTF-8 with no control
// character. A saved layout's dashboard key holds such a name (tab_order.h).
std::string dashboardNameProblem(std::string_view name);

// Why `d` is not a dashboard a window can show, or empty when it is: a name of 1 to kDashboardNameMax bytes of UTF-8
// with no control character, and an http:// or https:// address with a host, of at most kDashboardUrlMax bytes, with
// no space or control character.
std::string dashboardProblem(const Dashboard& d);

// One config line's value, `<url> <name>` (the name is the rest of the line and may hold spaces), into `out`. False,
// with why, when it is malformed or dashboardProblem refuses it.
bool parseDashboardLine(std::string_view value, Dashboard* out, std::string* why);

// The dashboards of `list` a window shows a tab for, in the list's order: each that dashboardProblem passes, the first
// of a name given twice, and at most kDashboardsMax. Each one left out adds one line to `refused` saying which and why.
// The window takes a server's list (frame_host.h takeDashboards) and a state file's (main.cpp) through it.
std::vector<Dashboard> shownDashboards(const std::vector<Dashboard>& list, std::vector<std::string>* refused);

} // namespace nexus::sync
