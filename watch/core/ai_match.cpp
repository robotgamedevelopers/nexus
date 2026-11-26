#include "ai_match.h"

#include "voice/name_match.h"

#include <algorithm>

namespace nexus::watch {

namespace {

std::size_t indexOf(const std::vector<std::string>& ais, const std::string& name) {
    return static_cast<std::size_t>(std::find(ais.begin(), ais.end(), name) - ais.begin());
}

} // namespace

AiMatch matchAi(std::string_view transcript, const std::vector<std::string>& ais) {
    std::vector<voice::SpokenAI> spoken;
    spoken.reserve(ais.size());
    for (const std::string& a : ais)
        spoken.push_back(voice::SpokenAI{a, {}});
    const voice::Routing r = voice::route(transcript, spoken);
    AiMatch m;
    switch (r.route) {
    case voice::Route::Named:
        m.kind = AiMatch::Kind::Named;
        m.index = indexOf(ais, r.aiName);
        m.rest = r.prompt;
        break;
    case voice::Route::Current:
        m.rest = r.prompt;
        break;
    case voice::Route::Ambiguous:
        m.kind = AiMatch::Kind::Ambiguous;
        for (const std::string& c : r.candidates)
            m.candidates.push_back(indexOf(ais, c));
        std::sort(m.candidates.begin(), m.candidates.end());
        break;
    }
    return m;
}

} // namespace nexus::watch
