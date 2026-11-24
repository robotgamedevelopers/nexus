#include "chat_markdown.h"

#include "pr/markdown.h"

#include <algorithm>
#include <memory>

namespace nexus {
namespace {

struct Writer {
    ChatMarkdown paragraphs;
    struct List { bool ordered; uint64_t next; size_t firstParagraph, columns; };
    std::vector<List> lists;
    std::string marker, href;
    int strong = 0, emphasis = 0, code = 0, codeBlock = 0, quote = 0;
    unsigned heading = 0;
    bool newParagraph = true;
    unsigned cells = 0;

    void put(std::string_view text) {
        if (text.empty()) return;
        if (newParagraph) {
            paragraphs.emplace_back();
            newParagraph = false;
            auto& paragraph = paragraphs.back();
            paragraph.depth = lists.size();
            paragraph.marker.swap(marker);
            if (quote) paragraph.runs.push_back({"│ ", PrKitTextRole_Body, {}});

        }
        const auto role = code || codeBlock ? PrKitTextRole_Mono : heading == 1 ? PrKitTextRole_Title :
            heading ? PrKitTextRole_Heading : strong ? PrKitTextRole_Strong :
            emphasis ? PrKitTextRole_BodyItalic : PrKitTextRole_Body;
        auto& runs = paragraphs.back().runs;
        if (!runs.empty() && runs.back().role == role && runs.back().href == href && runs.back().inlineCode == (code > 0)) runs.back().text.append(text);
        else runs.push_back({std::string(text), static_cast<PrKitTextRole>(role), href, code > 0});
    }

    void visit(const PrMarkdownStep& s) {
        if (s.kind == PrMarkdownStepKind_Text) {
            put({s.text.data, s.text.size});
            return;
        }
        const bool enter = s.kind == PrMarkdownStepKind_Enter;
        const int delta = enter ? 1 : -1;
        switch (s.node) {
        case PrMarkdownNodeKind_CodeBlock: newParagraph = true; codeBlock += delta; break;
        case PrMarkdownNodeKind_Code: code += delta; break;
        case PrMarkdownNodeKind_Heading: heading = enter ? s.level : 0; newParagraph = true; break;
        case PrMarkdownNodeKind_BlockQuote: quote += delta; newParagraph = true; break;
        case PrMarkdownNodeKind_Strong: strong += delta; break;
        case PrMarkdownNodeKind_Emphasis: emphasis += delta; break;
        case PrMarkdownNodeKind_Paragraph:
        case PrMarkdownNodeKind_HtmlBlock: newParagraph = true; break;
        case PrMarkdownNodeKind_ThematicBreak:
            newParagraph = true;
            if (enter) put("────");
            break;
        case PrMarkdownNodeKind_List:
            if (enter) lists.push_back({s.list == PrMarkdownListKind_Ordered, s.start, paragraphs.size(), 1});
            else {
                const auto list = lists.back();
                for (size_t i = list.firstParagraph; i < paragraphs.size(); ++i) {
                    auto& p = paragraphs[i];
                    if (p.depth == lists.size()) p.markerColumns = list.columns;
                    else p.ancestorColumns += list.columns;
                }
                lists.pop_back();
            }
            break;
        case PrMarkdownNodeKind_Item:
            newParagraph = true;
            if (enter && !lists.empty()) {
                auto& list = lists.back();
                marker = list.ordered ? std::to_string(list.next++) + "." : "•";
                size_t columns = list.ordered ? marker.size() : 1;
                if (s.task != PrMarkdownTask_None) {
                    marker += s.task == PrMarkdownTask_Checked ? " ☑" : " ☐";
                    columns += 2;
                }
                marker += " ";
                list.columns = std::max(list.columns, columns);
            } else marker.clear();
            break;
        case PrMarkdownNodeKind_TableRow: newParagraph = true; cells = 0; break;
        case PrMarkdownNodeKind_TableCell:
            if (enter && cells++) put("   │   ");
            if (s.header) strong += delta;
            break;
        case PrMarkdownNodeKind_SoftBreak: if (enter) put(" "); break;
        case PrMarkdownNodeKind_HardBreak: if (enter) put("\n"); break;
        case PrMarkdownNodeKind_Link: {
            href.clear();
            if (enter && s.destination.size) {
                const std::string_view url{s.destination.data, s.destination.size};
                if (url.starts_with("https://") || url.starts_with("http://")) href = url;
            }
            break;
        }
        case PrMarkdownNodeKind_Image:
            if (!enter && s.destination.size) {
                put(" ("); put({s.destination.data, s.destination.size}); put(")");
            }
            break;
        default: break;
        }
    }
};

} // namespace

const ChatMarkdown& ChatMarkdownCache::get(std::string_view source) {
    const auto found = std::find_if(messages_.begin(), messages_.end(),
        [&](const Message& message) { return message.source == source; });
    if (found != messages_.end()) {
        messages_.splice(messages_.begin(), messages_, found);
        return messages_.front().paragraphs;
    }
    PrMarkdownOptions options{};
    options.extensions = PrMarkdownExtensionFlag_TaskLists | PrMarkdownExtensionFlag_Tables | PrMarkdownExtensionFlag_Autolinks;
    options.rawHtml = PrMarkdownRawHtml_Escape;
    options.destinations = PrMarkdownDestinations_Safe;
    std::unique_ptr<PrMarkdown, decltype(&prMarkdownDestroy)> doc(prMarkdownCreate(&options), prMarkdownDestroy);
    prMarkdownParse(doc.get(), {source.data(), source.size()});
    Writer writer;
    prMarkdownWalk(doc.get(), 0, prMarkdownBlockCount(doc.get()),
        [](void* out, const PrMarkdownStep* step) { static_cast<Writer*>(out)->visit(*step); }, &writer);
    messages_.push_front({std::string(source), std::move(writer.paragraphs)});
    if (messages_.size() > 32) messages_.pop_back();
    return messages_.front().paragraphs;
}

} // namespace nexus
