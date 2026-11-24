#pragma once

#include "pr/kit_text.h"

#include <list>
#include <string>
#include <string_view>
#include <vector>

namespace nexus {

struct ChatRun {
    std::string text;
    PrKitTextRole role;
    std::string href;
    bool inlineCode = false;
};

struct ChatParagraph {
    std::vector<ChatRun> runs;
    std::string marker;
    size_t depth = 0, ancestorColumns = 0, markerColumns = 0;
};
using ChatMarkdown = std::vector<ChatParagraph>;

// Cache only recently displayed source. Queued messages and growing replies can change between frames.
class ChatMarkdownCache {
public:
    const ChatMarkdown& get(std::string_view source);
private:
    struct Message { std::string source; ChatMarkdown paragraphs; };
    std::list<Message> messages_;
};

} // namespace nexus
