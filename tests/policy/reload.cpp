// Reuse both production hosts while replacing their activated bundles.
#include "policy/host.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    namespace fs = std::filesystem;
    using namespace nexus::policy;
    const fs::path root(argv[1]);
    Host client("client"), server("server");
    const auto c = (root / "client.json").string();
    const auto s = (root / "server.json").string();
    const auto zoom = [&] { return textZoom(client, c, 0, 1, -3, 4); };
    std::string why;
    const auto account = [&] {
        return accountAfterLimit(server, s, "a", {"a", "b", "c", "offline"}, {"b", "c"}, why);
    };
    if (zoom() != 1 || account() != "b" || !why.empty())
        return 2;
    const auto apply = [&](const char* revision, const char* side, const std::string& dest) {
        fs::copy_file(root / revision / side / "policy.bundle.json", dest, fs::copy_options::overwrite_existing);
    };
    apply("edit", "client", c);
    if (zoom() != 3 || account() != "b")
        return 3;
    apply("edit", "server", s);
    if (account() != "c" || !why.empty())
        return 4;
    apply("invalid", "client", c);
    apply("invalid", "server", s);
    if (zoom() != 0 || !account().empty() || why.empty())
        return 5;
    fs::remove(c);
    fs::remove(s);
    return zoom() == 1 && account() == "b" && why.empty() ? 0 : 6;
}
