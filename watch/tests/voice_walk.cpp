// The voice turn's walk (watch/core/voice_turn.h), for VoiceTurnTrace.tla: a seeded random walk of events through the
// real VoiceTurn, writing each event, its arguments, what the code returned and the code's state after it as a TLA+
// module TLC checks is a behaviour of VoiceTurn.tla. With a fault (built with NEXUS_WATCH_FAULTS), the code is planted
// wrong and the check must reject its trace.
//
//   voice_walk <seed> <steps> <out VoiceTurnTraceData.tla> [silent-mic | unread | drop-on-back]

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "voice_turn.h"

using namespace nexus::watch;

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string quoted(const std::string& s) {
    return "\"" + s + "\"";
}

std::string state(const VoiceTurn& t) {
    std::ostringstream o;
    o << "[phase |-> " << quoted(phaseName(t.phase())) << ", connected |-> " << (t.connected() ? "TRUE" : "FALSE")
      << ", current |-> " << quoted(lower(t.current())) << ", utterance |-> " << t.utterance()
      << ", heard |-> " << t.heard() << ", unapplied |-> {";
    for (std::size_t k = 0; k < t.unapplied().size(); ++k)
        o << (k ? ", " : "") << t.unapplied()[k];
    o << "}, nextPrompt |-> " << t.nextPrompt() << ", awaiting |-> " << t.awaiting()
      << ", replies |-> " << t.replies().size() << ", notices |-> " << t.notices() << ", cues |-> " << t.cues()
      << ", unlocked |-> " << (t.unlocked() ? "TRUE" : "FALSE") << "]";
    return o.str();
}

// The model's name for what the matcher made of the words (VoiceTurn.tla, Routes).
std::string routeOf(const AiMatch& m, const std::vector<std::string>& ais) {
    switch (m.kind) {
    case AiMatch::Kind::Ambiguous: return "ambiguous";
    case AiMatch::Kind::Named: return m.rest.empty() ? "nameonly" : "named";
    case AiMatch::Kind::None: break;
    }
    return m.rest.empty() ? "empty" : ais.empty() ? "noais" : "none";
}

std::string record(const std::string& act, bool ok, bool b, std::uint64_t n, const std::string& route,
                   const std::string& f, const VoiceTurn& t) {
    std::ostringstream o;
    o << "[act |-> " << quoted(act) << ", ok |-> " << (ok ? "TRUE" : "FALSE") << ", b |-> " << (b ? "TRUE" : "FALSE")
      << ", n |-> " << n << ", route |-> " << quoted(route) << ", f |-> " << quoted(lower(f)) << ", s |-> " << state(t)
      << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: voice_walk <seed> <steps> <out.tla> [fault]\n");
        return 2;
    }
    const unsigned long seed = std::strtoul(argv[1], nullptr, 10);
    const long steps = std::strtol(argv[2], nullptr, 10);
    if (argc == 5) {
#ifdef NEXUS_WATCH_FAULTS
        gWatchFault = argv[4];
#else
        std::fprintf(stderr, "voice_walk: built without NEXUS_WATCH_FAULTS: no fault %s\n", argv[4]);
        return 2;
#endif
    }
    std::mt19937_64 rng(seed);
    const auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
    const auto chance = [&](unsigned percent) { return pick(100) < percent; };

    const std::vector<std::string> ais{"Emma", "Emmy", "Bo"};
    const std::vector<std::string> texts{"Emma, how is the build?", "Emmy", "Emmo, hi", "what about the tests", "",
                                         "Bo, ok", "Bo", "hey Emmy, lunch?"};
    VoiceTurn t;
    t.setAis(ais);
    std::vector<std::string> trace{record("init", true, false, 0, "", "", t)};
    Ms now = 0, pressedAt = 0, deadline = 0;
    // The events, weighted so that turns run to their end as often as they are cut.
    for (long k = 0; k < steps; ++k) {
        const std::size_t e = pick(19);
        std::string act;
        bool ok = true, b = false;
        std::uint64_t n = 0;
        std::string route, f;
        if (e <= 2) {
            act = "press";
            b = chance(85);
            now += 10;
            ok = t.press(now, b);
            if (ok && t.phase() == Phase::Listening)
                pressedAt = now;
        } else if (e <= 4) {
            act = "release";
            b = chance(75);
            now = std::max(now, pressedAt + (b ? 800 : 50));
            b = now >= pressedAt + VoiceTurn::kMinTalkMs;
            ok = t.release(now);
            if (t.phase() == Phase::Transcribing)
                deadline = now + VoiceTurn::kTranscribeMs;
        } else if (e <= 6) {
            act = "transcript";
            n = chance(80) ? t.heard() : pick(t.heard() + 2);
            const Phase before = t.phase();
            ok = t.transcript(n, texts[pick(texts.size())]);
            // Choosing is where the app chooses at once (nw_take): most often the next event.
            if (ok && before == Phase::Transcribing && chance(85)) {
                trace.push_back(record(act, ok, b, n, route, f, t));
                act = "choose";
                n = 0;
                t.choose();
                const AiMatch& m = t.lastMatch();
                route = routeOf(m, ais);
                f = m.kind == AiMatch::Kind::Named              ? ais[m.index]
                    : m.kind == AiMatch::Kind::None && !ais.empty() ? ais[0]
                                                                    : "";
            }
        } else if (e == 7) {
            act = "failed";
            n = chance(80) ? t.heard() : pick(t.heard() + 2);
            ok = t.transcriptFailed(n, "no words");
        } else if (e == 8) {
            act = "choose";
            ok = t.phase() == Phase::Choosing;
            t.choose();
            if (ok) {
                const AiMatch& m = t.lastMatch();
                route = routeOf(m, ais);
                f = m.kind == AiMatch::Kind::Named              ? ais[m.index]
                    : m.kind == AiMatch::Kind::None && !ais.empty() ? ais[0]
                                                                    : "";
            }
        } else if (e <= 10 && !t.unapplied().empty() && t.connected() && chance(40)) {
            // The server lost while a prompt is in flight, the case a reconnect must keep it through.
            act = "lost";
            t.lost("gone");
            if (chance(60)) {
                trace.push_back(record(act, ok, b, n, route, f, t));
                act = "back";
                ok = t.back();
            }
        } else if (e <= 10) {
            act = "applied";
            n = pick(t.nextPrompt() + 1);
            ok = t.applied(n);
        } else if (e == 11) {
            act = "lostprompt";
            n = !t.unapplied().empty() && chance(70) ? t.unapplied()[pick(t.unapplied().size())] : pick(t.nextPrompt() + 1);
            ok = t.lostPrompt(n);
        } else if (e == 12) {
            act = "reply";
            t.reply(ais[pick(ais.size())], "words", chance(20));
        } else if (e == 13) {
            act = chance(70) ? "spoken" : chance(50) ? "resume" : "dismiss";
            ok = act == "spoken" ? t.speechDone() : act == "resume" ? t.resume() : t.dismiss();
        } else if (e == 14) {
            act = chance(50) ? "lost" : "back";
            if (act == "lost")
                t.lost("gone");
            else
                ok = t.back();
        } else if (e == 15) {
            act = "back";
            ok = t.back();
        } else if (e == 17) {
            // The owner's check: locked now and then (off the wrist), unlocked again soon after.
            act = chance(40) ? "lock" : "unlock";
            if (act == "lock")
                t.lock();
            else
                t.unlock();
        } else if (e == 18) {
            act = "unlock";
            t.unlock();
        } else {
            act = "tick";
            b = chance(50);
            if (b)
                now = std::max(now, deadline);
            b = now >= deadline;
            ok = t.tick(now);
        }
        trace.push_back(record(act, ok, b, n, route, f, t));
    }

    std::ofstream out(argv[3]);
    out << "---------------------------- MODULE VoiceTurnTraceData ----------------------------\n"
        << "\\* Written by voice_walk " << argv[1] << " " << argv[2] << (argc == 5 ? std::string(" ") + argv[4] : "")
        << ": one walk of the real VoiceTurn.\n"
        << "TraceAis == {\"emma\", \"emmy\", \"bo\"}\n"
        << "Trace == <<\n";
    for (std::size_t k = 0; k < trace.size(); ++k)
        out << "  " << trace[k] << (k + 1 < trace.size() ? ",\n" : "\n");
    out << ">>\n=============================================================================\n";
    return out ? 0 : 1;
}
