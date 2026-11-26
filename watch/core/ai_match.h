// Which AI a spoken prompt is for, worked out on the device from what was said and the AIs list the server sends
// (docs/watch.md). The matcher is the iPhone's (src/voice/name_match.h, nexus::voice::route), shared so the phone and
// the watch route the same words to the same AI; this is the watch's view of its answer, by index into the AIs list.
//
// Logic only: no clock, no I/O.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::watch {

struct AiMatch {
    enum class Kind {
        None,       // no AI is named: the words are for the current AI
        Named,      // `index` is the AI named; `rest` the prompt less the name (may be empty: a name alone)
        Ambiguous,  // two or more AIs answer equally: `candidates`, in the list's order; nothing is sent
    };
    Kind kind = Kind::None;
    std::size_t index = 0;
    std::string rest;                   // with Named and None: the prompt
    std::vector<std::size_t> candidates;
};

// The AI `transcript` names among `ais` (the server's names), by nexus::voice::route.
AiMatch matchAi(std::string_view transcript, const std::vector<std::string>& ais);

} // namespace nexus::watch
