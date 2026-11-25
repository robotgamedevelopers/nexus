#include "sync/dashboards.h"

#include "sync/codec.h"

namespace nexus::sync {

namespace {

bool control(unsigned char c) {
    return c < 0x20 || c == 0x7f;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

} // namespace

std::string dashboardNameProblem(std::string_view name) {
    if (name.empty() || name.size() > kDashboardNameMax)
        return "a dashboard's name is 1 to " + std::to_string(kDashboardNameMax) + " bytes";
    if (!codec::tableText(name))
        return "the dashboard name \"" + std::string(name) + "\" is not UTF-8";
    for (unsigned char c : name)
        if (control(c)) return "the dashboard name \"" + std::string(name) + "\" holds a control character";
    return {};
}

std::string dashboardProblem(const Dashboard& d) {
    if (std::string problem = dashboardNameProblem(d.name); !problem.empty())
        return problem;
    const std::string_view url = d.url;
    std::string_view rest;
    if (startsWith(url, "http://")) rest = url.substr(7);
    else if (startsWith(url, "https://")) rest = url.substr(8);
    else return "the dashboard " + d.name + "'s address " + d.url + " is not http:// or https://";
    if (rest.empty() || rest.front() == '/' || rest.front() == ':')
        return "the dashboard " + d.name + "'s address " + d.url + " has no host";
    if (url.size() > kDashboardUrlMax)
        return "the dashboard " + d.name + "'s address is longer than " + std::to_string(kDashboardUrlMax) + " bytes";
    for (unsigned char c : url)
        if (control(c) || c == ' ' || c >= 0x80)
            return "the dashboard " + d.name + "'s address holds a space, a control character or a non-ASCII byte";
    return {};
}

bool parseDashboardLine(std::string_view value, Dashboard* out, std::string* why) {
    const std::size_t space = value.find_first_of(" \t");
    std::string_view name = space == std::string_view::npos ? std::string_view() : value.substr(space + 1);
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
    if (space == std::string_view::npos || name.empty()) {
        *why = "a dashboard is `dashboard <url> <name>`: the address, then the tab's name";
        return false;
    }
    Dashboard d{std::string(name), std::string(value.substr(0, space))};
    if (std::string problem = dashboardProblem(d); !problem.empty()) {
        *why = std::move(problem);
        return false;
    }
    *out = std::move(d);
    return true;
}

std::vector<Dashboard> shownDashboards(const std::vector<Dashboard>& list, std::vector<std::string>* refused) {
    std::vector<Dashboard> shown;
    for (const Dashboard& d : list) {
        std::string problem = dashboardProblem(d);
        bool twice = false;
        for (const Dashboard& s : shown)
            twice = twice || s.name == d.name;
        if (!problem.empty())
            refused->push_back("a dashboard this window cannot show (" + problem + "): it has no tab");
        else if (twice)
            refused->push_back("the dashboard \"" + d.name + "\" twice: the first has its tab");
        else if (shown.size() == kDashboardsMax)
            refused->push_back("more than the " + std::to_string(kDashboardsMax) + " dashboards a window shows: \"" +
                d.name + "\" has no tab");
        else
            shown.push_back(d);
    }
    return shown;
}

} // namespace nexus::sync
