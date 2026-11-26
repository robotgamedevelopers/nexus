// watch_match <ai>[,<ai>...] <transcript>: what the watch's matcher makes of a transcript, one line, for the speech
// test (speech.cmake), which feeds it the recognizer's words: "named <ai>|<rest>", "none|<rest>" or
// "ambiguous <ai>,<ai>".

#include "ai_match.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: watch_match <ai>[,<ai>...] <transcript>\n");
        return 2;
    }
    std::vector<std::string> ais;
    std::string list = argv[1];
    for (std::size_t at = 0; at <= list.size();) {
        const std::size_t comma = list.find(',', at);
        const std::size_t end = comma == std::string::npos ? list.size() : comma;
        ais.push_back(list.substr(at, end - at));
        at = end + 1;
    }
    const nexus::watch::AiMatch m = nexus::watch::matchAi(argv[2], ais);
    switch (m.kind) {
    case nexus::watch::AiMatch::Kind::Named:
        std::printf("named %s|%s\n", ais[m.index].c_str(), m.rest.c_str());
        break;
    case nexus::watch::AiMatch::Kind::None:
        std::printf("none|%s\n", m.rest.c_str());
        break;
    case nexus::watch::AiMatch::Kind::Ambiguous:
        std::printf("ambiguous");
        for (std::size_t i = 0; i < m.candidates.size(); ++i)
            std::printf("%s%s", i ? "," : " ", ais[m.candidates[i]].c_str());
        std::printf("\n");
        break;
    }
    return 0;
}
