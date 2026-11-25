// Where the sprint board is served, for the windows' sprint tab (docs/formats.md, SessionsDiff.sprint_url): the
// address of `nova-sprint run --listen HOST:PORT`, read off its argv as the sprint's run service records it, whether
// that record comes from `nova-config service show sprint-run --json` (a window, sprint.h) or from the services
// nexus-server runs itself (server/service_manifest.h). Pure.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nexus::sync {

// The name of the sprint's run service, in nova-config and in nexus-server's services.json.
constexpr std::string_view kSprintRunService = "sprint-run";

// The page address of a --listen value: http://HOST:PORT/, with a wildcard or missing host read as 127.0.0.1.
std::string sprintListenUrl(std::string_view listen);

// The board's address from the run service's argv (`--listen V` or `--listen=V`), or empty when it has none.
std::string sprintBoardFromArgv(const std::vector<std::string>& argv);

} // namespace nexus::sync
