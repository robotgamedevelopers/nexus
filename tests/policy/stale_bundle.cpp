// The warning a host gives when its applied policy bundle is not the one its binary packages (policy/host.h
// staleBundle). Files go in the working directory only; no time, sockets or network.
//
//   policy_stale_bundle_test            the comparison: no bundle, equal, a differing source, a differing config
//   policy_stale_bundle_test DIR        the bundles `nexus-policy apply <side> policy/rules DIR/<side>` wrote are equal
#include "policy/host.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* side, const char* what, const std::string& got) {
    if (ok)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s (got: %s)\n", side, what, got.empty() ? "nothing" : got.c_str());
}

void write(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void comparison(const char* side) {
    using nexus::policy::staleBundle;
    const std::string path = std::string("stale-") + side + ".bundle.json";
    std::remove(path.c_str());

    // No bundle: the packaged rules run, and there is nothing to say.
    auto said = staleBundle(side, path);
    check(said.empty(), side, "no bundle is not stale", said);

    std::string why;
    const std::string packaged = nexus::policy::packagedBundle(side, why);
    check(why.empty() && !packaged.empty(), side, "the packaged bundle is readable", why);

    // Equal: what apply writes from this commit.
    write(path, packaged);
    said = staleBundle(side, path);
    check(said.empty(), side, "the packaged bundle is not stale", said);

    // A differing source: one more line at the start of the first module.
    const auto sources = packaged.find("\"sources\":{\"");
    const auto value = sources == std::string::npos ? sources : packaged.find("\":\"", sources + 12);
    check(value != std::string::npos, side, "the packaged bundle has a source", packaged.substr(0, 80));
    if (value != std::string::npos) {
        std::string edited = packaged;
        edited.insert(value + 3, "# an older rule\\n");
        write(path, edited);
        said = staleBundle(side, path);
        check(contains(said, "differs") && contains(said, path) && contains(said, "nexus-policy apply ") &&
                  contains(said, std::string("apply ") + side + " policy/rules"),
            side, "a differing source is stale, naming the file and the fix", said);
    }

    // A differing config: one more setting.
    const auto config = packaged.find("\"config\":{");
    check(config != std::string::npos, side, "the packaged bundle has a config", packaged.substr(0, 80));
    if (config != std::string::npos) {
        std::string edited = packaged;
        edited.insert(config + 10, "\"stale_setting\":true,");
        write(path, edited);
        said = staleBundle(side, path);
        check(contains(said, "config differs") && contains(said, path), side, "a differing config is stale", said);
    }

    std::remove(path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        for (const char* side : {"client", "server"}) {
            const std::string path = std::string(argv[1]) + "/" + side + "/policy.bundle.json";
            if (!std::ifstream(path)) {
                check(false, side, "apply wrote a bundle", path);
                continue;
            }
            const auto said = nexus::policy::staleBundle(side, path);
            check(said.empty(), side, "the bundle apply writes from policy/rules is not stale", said);
        }
    } else {
        comparison("client");
        comparison("server");
    }
    if (failures == 0)
        std::printf("ok\n");
    return failures == 0 ? 0 : 1;
}
