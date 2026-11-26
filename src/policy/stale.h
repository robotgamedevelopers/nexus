#pragma once

#include <string>

namespace nexus::policy {

// An applied bundle runs in place of the rules this binary packages. When the bundle at `path` (a host's
// policy.bundle.json) is not what `nexus-policy apply <side> policy/rules` writes from this build's commit, by content,
// this returns one line naming the file, how it differs, and the fix; it returns an empty string when the file is
// absent or the same. It warns; nothing refuses to start over it. Its own header, so a change here rebuilds only the
// few files that call it.
std::string staleBundle(const char* side, const std::string& path);
// The bundle `nexus-policy apply <side> policy/rules` writes from this build's commit, as JSON (for the tests).
std::string packagedBundle(const char* side, std::string& why);

}  // namespace nexus::policy
