#include "config.h"

#include <algorithm>

namespace nexus {

namespace {

bool space(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::string known(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return "it takes no settings yet";
    }
    std::string out = "the keys are";
    for (const std::string& k : keys) {
        out += " `" + k + "`";
    }
    return out;
}

} // namespace

Config parseConfig(std::string_view text, const std::string& path, const std::vector<std::string>& keys) {
    Config config;
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::size_t end = 0;
        while (end < line.size() && !space(line[end])) {
            ++end;
        }
        const std::string key(line.substr(0, end));
        const std::string value(trim(line.substr(end)));
        const std::string where = path + " line " + std::to_string(number) + ": ";
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            config.warnings.push_back(where + "\"" + key + "\" is not a setting, so the line is skipped; " +
                known(keys) + "; remove the line or fix the key");
            continue;
        }
        if (value.empty()) {
            config.errors.push_back(where + "`" + key + "` has no value, so the line is not used; write it as `" + key +
                " <value>`");
            continue;
        }
        if (config.values.count(key) != 0) {
            config.errors.push_back(where + "`" + key + "` is set again, so the line is not used; keep one `" + key +
                "` line");
            continue;
        }
        config.values[key] = value;
    }
    return config;
}

} // namespace nexus
