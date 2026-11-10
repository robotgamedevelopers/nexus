#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace nexus {

struct FriendInfo { std::string name; std::string model; };

enum class EntryKind { Person, Friend, Notice };

struct Entry { EntryKind kind; std::string text; };

struct Tab {
    std::string name;
    std::string model;
    std::vector<Entry> transcript;
    std::string draft;
    std::vector<std::string> queue;  // prompts queued for later, oldest first; apart from the transcript
};

class Model {
public:
    explicit Model(const std::vector<FriendInfo>& friends); // one tab per friend, in order; active tab 0; may be empty
    std::size_t tab_count() const;
    std::size_t active() const;                  // 0 when there are no tabs
    const Tab& tab(std::size_t i) const;         // i < tab_count()
    bool select_tab(std::size_t i);              // false and no change when i >= tab_count()
    bool set_draft(std::size_t i, std::string text); // false and no change when i >= tab_count()
    bool send(std::size_t i);   // draft not all-whitespace: append {Person, draft} to the transcript, clear that draft, true;
                                // else false, no change. Never touches a queue.
    bool queue(std::size_t i);  // draft not all-whitespace: append the draft to that tab's queue, clear that draft, true;
                                // else false, no change. Never touches a transcript.
    bool append(std::size_t i, Entry e);         // false and no change when i >= tab_count()

private:
    bool sendable(std::size_t i) const;
    std::vector<Tab> tabs_;
    std::size_t active_ = 0;
};

} // namespace nexus
