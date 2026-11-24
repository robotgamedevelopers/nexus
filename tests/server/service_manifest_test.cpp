#include "server/service_manifest.h"

#include <cstdio>
#include <string>
#include <vector>

using nexus::server::ManagedService;
using nexus::server::parseServiceManifest;

int main() {
    const std::string fields = R"("name":"worker","owner":"nexus-server","cwd":"/srv/test","log":"/tmp/test.log","env":{"HOME":"/srv/test","PATH":"/bin:/usr/bin"})";
    const std::string args = R"("argv":["worker","space and \"quote\"","$unchanged",""])";
    const auto manifest = [](const std::string& rows) { return "{\"version\":1,\"services\":[" + rows + "]}"; };
    const auto row = [&](const std::string& extra) { return "{" + fields + "," + args + extra + "}"; };
    const std::string external = R"({"name":"hostjob","owner":"host"},{"name":"unassigned"},{"name":"paused","owner":"nexus-server","enabled":false})";
    std::vector<ManagedService> services;
    std::string why;
    if (!parseServiceManifest(manifest(external + "," + row(",\"every\":2")), services, why) ||
        services.size() != 1 || services[0].everySeconds != 2 ||
        services[0].argv != std::vector<std::string>{"worker", "space and \"quote\"", "$unchanged", ""}) {
        std::fprintf(stderr, "valid service list: %s\n", why.c_str());
        return 1;
    }
    const auto accepted = services;
    for (const auto& invalid : std::vector<std::string>{
        manifest(row(",\"every\":1.5")), manifest(row(",\"every\":-1")), manifest(row(",\"every\":4294967296")),
        manifest(row(",\"env\":{}")), manifest(row(",\"owenr\":\"host\"")), manifest(row(",\"enabled\":0")),
        manifest(row(",\"every\":2") + "," + row(",\"every\":1")),
        manifest("{" + fields + R"(,"argv":["bad\u0000word"]})"),
        manifest("{" + fields + R"(,"argv":["relative/exec"]})"),
        "{\"version\":2,\"services\":[]}", "{\"version\":true,\"services\":[]}",
        std::string(1024 * 1024 + 1, ' ')}) {
        if (parseServiceManifest(invalid, services, why) || services != accepted || why.empty()) {
            std::fprintf(stderr, "invalid manifest replaced the live configuration\n");
            return 1;
        }
    }
    if (!parseServiceManifest(manifest(""), services, why) || !services.empty()) return 1;
    return 0;
}
