#include "friends.h"

#include <cstdio>

namespace nexus {

namespace {

// Reads the JSON string value of `key` in `json` starting at `from`, before `end`. Returns false if absent.
bool readString(const std::string& json, const char* key, size_t from, size_t end, std::string& out) {
    std::string quoted = std::string("\"") + key + "\"";
    size_t at = json.find(quoted, from);
    if (at == std::string::npos || at >= end)
        return false;
    size_t colon = json.find(':', at + quoted.size());
    size_t open = colon == std::string::npos ? colon : json.find('"', colon);
    if (open == std::string::npos || open >= end)
        return false;
    out.clear();
    for (size_t i = open + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            out.push_back(json[++i]);
            continue;
        }
        if (json[i] == '"')
            return true;
        out.push_back(json[i]);
    }
    return false;
}

std::vector<Friend> placeholders() {
    return {{"friend one", "model"}, {"friend two", "model"}, {"friend three", "model"}, {"friend four", "model"}};
}

} // namespace

std::vector<Friend> loadFriends(std::string& note) {
    FILE* pipe = popen("nova-config friend list --json 2>/dev/null", "r");
    if (pipe == nullptr) {
        note = "could not start nova-config";
        return placeholders();
    }
    std::string json;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof buffer, pipe)) > 0)
        json.append(buffer, got);
    int status = pclose(pipe);
    if (status != 0) {
        note = "nova-config friend list --json exited with status " + std::to_string(status);
        return placeholders();
    }
    // Each friend is an object; split on the "name" keys and read what follows each up to the next.
    std::vector<Friend> friends;
    size_t at = json.find("\"name\"");
    while (at != std::string::npos) {
        size_t next = json.find("\"name\"", at + 1);
        size_t end = next == std::string::npos ? json.size() : next;
        Friend f;
        std::string segment = json.substr(at, end - at);
        bool disabled = segment.find("\"enabled\": false") != std::string::npos ||
            segment.find("\"enabled\":false") != std::string::npos;
        if (readString(json, "name", at, end, f.name) && !disabled) {
            readString(json, "model", at, end, f.model);
            friends.push_back(f);
        }
        at = next;
    }
    if (friends.empty()) {
        note = "nova-config friend list --json listed no friends";
        return placeholders();
    }
    note = "read " + std::to_string(friends.size()) + " friends from nova-config";
    return friends;
}

} // namespace nexus
