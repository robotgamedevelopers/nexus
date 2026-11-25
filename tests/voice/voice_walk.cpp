// Drives the real nexus::voice::VoiceTurn (src/voice/voice_turn.h) with a seeded random walk and writes the trace as a
// TLA+ module (VoiceTurnTraceData.tla) for models/VoiceTurnTrace.tla, under which TLC checks that the trace is a
// behaviour of models/VoiceTurn.tla.
//
//   voice_walk <seed> <steps> [--faulty-turn-end] <out.tla>
//
// Two AIs, emma and rowan, who both answer to the alias "boss" (so that a call of it is ambiguous). Each step is one
// of: the microphone pressed, the OS's answer (granted or refused), the transcript as it forms or at its end (one of a
// fixed set of sentences, routed by the real matcher, name_match.h), a recognizer failure, a frame some seconds on, a
// block of an AI's reply, the end of an utterance, an AI's turn ending, the person's choice of text, voice or
// both, or the tab showing changing. The walk is a pure function of its arguments: its random numbers come from the
// seed (splitmix64), its time is seconds it counts itself, never the clock's. Each record holds the step's arguments,
// the acts the code returned, and its state after. A frame whose tick does nothing is not a step of the model and is not
// written.
//
// --faulty-turn-end walks FaultyTurn instead: a test-only turn whose turnEnded keeps the AI awaited, so a later
// reply is spoken. TLC must reject its traces; that is the negative check that shows the binding can fail.

#include "voice/voice_turn.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace nexus::voice;

// The test-only fault: an AI's turn ending is lost. Hides VoiceTurn::turnEnded; the walk is a template.
class FaultyTurn : public VoiceTurn {
public:
    void turnEnded(std::string_view) {}
};

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    int below(int n) { return int(next() % uint64_t(n)); }
};

const std::vector<SpokenAI> kAIs = {{"emma", {"boss"}}, {"rowan", {"boss"}}};
const char* const kSaid[] = {"", "what's left?", "Emma, status", "Rowan.", "hey Rowan, ship it", "Boss, hi",
    "status, Emma", "Emma"};
const char* const kCurrents[] = {"emma", "rowan", ""};
const double kDts[] = {0.5, 3, 30, 60};

std::string str(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string aiOr(std::string_view name) {
    return str(name.empty() ? "none" : name);
}

std::string outOf(std::string_view said) {
    const auto r = route(said, kAIs);
    std::ostringstream o;
    o << "[route |-> " << str(r.route == Route::Named ? "named" : r.route == Route::Ambiguous ? "ambiguous" : "current")
      << ", ai |-> " << aiOr(r.route == Route::Named ? r.aiName : "") << ", prompt |-> "
      << ((r.route == Route::Ambiguous || !r.prompt.empty()) ? "TRUE" : "FALSE") << "]";
    return o.str();
}

std::string actsOf(const std::vector<Act>& acts) {
    bool ask = false, start = false, stopListen = false, stopSpeak = false, draft = false;
    std::string speak, open, send;
    for (const auto& a : acts) {
        switch (a.kind) {
            case Act::Kind::AskPermission: ask = true; break;
            case Act::Kind::StartListening: start = true; break;
            case Act::Kind::StopListening: stopListen = true; break;
            case Act::Kind::StopSpeaking: stopSpeak = true; break;
            case Act::Kind::Speak: speak = a.aiName; break;
            case Act::Kind::Open: open = a.aiName; break;
            case Act::Kind::Send: send = a.aiName; break;
            case Act::Kind::Draft: draft = true; break;
        }
    }
    auto b = [](bool v) { return v ? "TRUE" : "FALSE"; };
    std::ostringstream o;
    o << "[ask |-> " << b(ask) << ", start |-> " << b(start) << ", stopListen |-> " << b(stopListen)
      << ", stopSpeak |-> " << b(stopSpeak) << ", speak |-> " << aiOr(speak) << ", open |-> " << aiOr(open)
      << ", send |-> " << aiOr(send) << ", draft |-> " << b(draft) << "]";
    return o.str();
}

template <typename Turn>
std::string codeOf(const Turn& t) {
    std::ostringstream o;
    const char* permission = t.permitted() == Permission::Granted ? "granted" :
                             t.permitted() == Permission::Refused ? "refused" : "unknown";
    o << "[phase |-> " << str(phaseName(t.phase())) << ", permission |-> " << str(permission) << ", reply |-> "
      << str(replyName(t.reply())) << ", speaking |-> " << (t.speaking() ? "TRUE" : "FALSE") << ", queue |-> <<";
    bool first = true;
    for (const auto& q : t.queued()) {
        o << (first ? "" : ", ") << str(q.aiName);
        first = false;
    }
    o << ">>, awaiting |-> {";
    first = true;
    for (const auto& f : t.awaiting()) {
        o << (first ? "" : ", ") << str(f);
        first = false;
    }
    o << "}, heard |-> " << (t.heardText().empty() ? "FALSE" : "TRUE") << "]";
    return o.str();
}

struct Step {
    std::string op = "init", aiName, tick, r = "both", out = outOf(""), cur = "\"none\"";
    bool granted = false, h = false;
};

template <typename Turn>
int walk(uint64_t seed, int steps, const char* path) {
    Rng rng{seed};
    Turn t;
    double now = 0;
    std::string current = "emma";
    t.context(current, kAIs);
    std::vector<std::string> records;
    auto record = [&](const Step& s, const std::vector<Act>& acts) {
        std::ostringstream o;
        o << "[op |-> " << str(s.op) << ", ai |-> " << aiOr(s.aiName) << ", granted |-> "
          << (s.granted ? "TRUE" : "FALSE") << ", h |-> " << (s.h ? "TRUE" : "FALSE") << ", out |-> " << s.out
          << ", cur |-> " << s.cur << ", r |-> " << str(s.r) << ", tick |-> " << str(s.tick) << ", acts |-> "
          << actsOf(acts) << ", code |-> " << codeOf(t) << "]";
        records.push_back(o.str());
    };
    record(Step{}, {});
    for (int i = 0; i < steps; ++i) {
        Step s;
        std::vector<Act> acts;
        const int pick = rng.below(22);
        if (pick < 4) {
            s.op = "press";
            acts = t.press(now);
        } else if (pick < 6) {
            s.op = "permission";
            s.granted = rng.below(2) != 0;
            acts = t.permission(s.granted, "");
        } else if (pick < 8) {
            s.op = "partial";
            const char* said = kSaid[rng.below(8)];
            s.h = said[0] != '\0';
            acts = t.heard(said, false);
        } else if (pick < 11) {
            s.op = "final";
            const char* said = kSaid[rng.below(8)];
            s.out = outOf(said);
            s.cur = aiOr(current);
            acts = t.heard(said, true);
        } else if (pick < 12) {
            s.op = "fail";
            acts = t.failed("The audio session was interrupted.");
        } else if (pick < 14) {
            now += kDts[rng.below(4)];
            const bool finishing = t.phase() == Phase::Finishing;
            s.out = outOf(t.heardText());
            s.cur = aiOr(current);
            const Phase before = t.phase();
            acts = t.tick(now);
            if (acts.empty() && t.phase() == before) continue; // nothing was due: no step
            s.op = "tick";
            s.tick = finishing ? "finish" : "stop";
        } else if (pick < 17) {
            s.op = "reply";
            s.aiName = kAIs[std::size_t(rng.below(2))].name;
            acts = t.replied(s.aiName, "a block");
        } else if (pick < 19) {
            s.op = "spoken";
            acts = t.spoken();
        } else if (pick < 20) {
            s.op = "turn_end";
            s.aiName = kAIs[std::size_t(rng.below(2))].name;
            t.turnEnded(s.aiName);
        } else if (pick < 21) {
            s.op = "set_reply";
            const Reply r = rng.below(3) == 0 ? Reply::Text : rng.below(2) ? Reply::Voice : Reply::Both;
            s.r = replyName(r);
            acts = t.setReply(r);
        } else {
            current = kCurrents[rng.below(3)]; // the person changed tabs: no step of the turn
            t.context(current, kAIs);
            continue;
        }
        record(s, acts);
    }
    std::ofstream file(path);
    if (!file) {
        std::fprintf(stderr, "voice_walk: cannot write %s\n", path);
        return 1;
    }
    file << "---- MODULE VoiceTurnTraceData ----\n";
    file << "\\* Written by tests/voice/voice_walk (seed " << seed << ", " << steps << " steps): one trace of the real "
            "nexus::voice::VoiceTurn.\n";
    file << "TraceAIs == {\"emma\", \"rowan\"}\n";
    file << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) file << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
    file << ">>\n====\n";
    std::printf("voice_walk: %zu records written to %s\n", records.size(), path);
    return file.good() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    bool faulty = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-turn-end") == 0) faulty = true;
        else args.push_back(argv[i]);
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: voice_walk <seed> <steps> [--faulty-turn-end] <out.tla>\n");
        return 2;
    }
    const uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const int steps = std::atoi(args[1]);
    return faulty ? walk<FaultyTurn>(seed, steps, args[2]) : walk<VoiceTurn>(seed, steps, args[2]);
}
