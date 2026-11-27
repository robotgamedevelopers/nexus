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

struct ChatCell {
    std::vector<ChatRun> runs;
    PrKitTextAlign align = PrKitTextAlign_Start;
};

struct ChatParagraph {
    std::vector<ChatRun> runs;
    std::string marker;
    size_t depth = 0, ancestorColumns = 0, markerColumns = 0;
    std::vector<std::vector<ChatCell>> tableRows;
    bool quoted = false;
};
std::string markdownSource(const ChatParagraph& paragraph);

using ChatMarkdown = std::vector<ChatParagraph>;

// Cache only recently displayed source. Queued messages and growing replies can change between frames.
class ChatMarkdownCache {
public:
    const ChatMarkdown& get(std::string_view source, bool preserveBreaks = false);
private:
    struct Message { std::string source; ChatMarkdown paragraphs; bool preserveBreaks; };
    std::list<Message> messages_;
};

} // namespace nexus
