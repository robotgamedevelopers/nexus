// nexus's own settings files: ~/.nexus/client/config for the window (client_home.h) and ~/.nexus/server/config for
// nexus-server (server/server.h). One setting per line, `<key> <value>`: the key is the line's first word, the value
// the rest of the line with the spaces around it removed (so a path may hold spaces). A line whose first word starts
// with `#` is a comment, and blank lines are skipped. A key with no value, or a key given twice, is an error naming the
// file and the line, and that line is not used; a well-formed line whose key the side does not know is a warning and
// is skipped. A list key (as the server's `dashboard`) is the one exception: it may be given on any number of lines,
// and each line is one item, in the file's order.
// Pure: parsing only, no files.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nexus {

struct Config {
    std::map<std::string, std::string> values;   // key: value, for the keys the side knows
    // list key: each line's value, in order, with its line number (for the messages)
    std::map<std::string, std::vector<std::pair<std::size_t, std::string>>> lists;
    std::vector<std::string> warnings;           // "<path> line <n>: ...", each with what to do
    std::vector<std::string> errors;             // "<path> line <n>: ...", each with what to do
};

// The settings in `text`, the file at `path` (for the messages), keeping only `keys` and `listKeys`.
Config parseConfig(std::string_view text, const std::string& path, const std::vector<std::string>& keys,
    const std::vector<std::string>& listKeys = {});

} // namespace nexus
