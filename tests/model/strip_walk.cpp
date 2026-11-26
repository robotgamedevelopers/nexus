// Drives the real nexus::sync::AIStrip (src/sync/ai_strip.h) over a real Model with a seeded random walk and
// writes the trace as a TLA+ module (AIStripTraceData.tla) for models/AIStripTrace.tla, under which TLC checks
// that the trace is a behaviour of models/AIStrip.tla.
//
//   strip_walk <seed> <steps> <ais 2-3> [--faulty-forget | --faulty-wait] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), and it reads no
// clock and no file. Each step is one event: a state arrives with a list of AIs, the server's own (exact) or an
// older server's sessions (which only add), good names and a bad one among them; the same state arrives again; the
// person types a draft or chooses a tab; or the window quits and starts again, its kept drafts handed to the next
// strip as main.cpp hands them from the file (tab_order.h), and the tab that showed saved as main.cpp saves it.
//
// --faulty-forget starts each new strip with no kept drafts, as a window that did not save them would; --faulty-wait
// starts it with no saved AI, as a window that judged the saved tab at launch would. TLC must reject the traces of
// both; they are the negative checks that show the binding can fail.

#include "model.h"
#include "sync/ai_strip.h"
#include "sync/sessions.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nexus::Model;
using nexus::TabKind;
using namespace nexus::sync;

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return std::size_t(next() % n); }
};

std::string str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

std::string seq(const std::vector<std::string>& names) {
    std::string out = "<<";
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += (i != 0 ? ", " : "") + str(names[i]);
    }
    return out + ">>";
}

template <class Map>
std::string function(const Map& m) {
    if (m.empty()) {
        return "[n \\in {} |-> \"\"]";
    }
    std::string out = "(";
    bool first = true;
    for (const auto& [name, text] : m) {
        out += (first ? "" : " @@ ") + str(name) + " :> " + str(text);
        first = false;
    }
    return out + ")";
}

const std::vector<std::string> kAllNames = {"a", "b", "c"};
const std::string kBad = "x y";
const std::vector<std::string> kTexts = {"", "t", "u"};

// The code's state, as AIStripTrace.tla's Matches reads it.
std::string codeOf(const Model& model, const AIStrip& strip) {
    std::vector<std::string> names;
    std::vector<std::pair<std::string, std::string>> drafts;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::AI) {
            names.push_back(model.tab(i).name);
            drafts.emplace_back(model.tab(i).name, model.tab(i).draft);
        }
    }
    const nexus::Tab& active = model.tab(model.active());
    const std::string selected = active.kind == TabKind::AI ? active.name : std::string();
    std::ostringstream o;
    o << "[strip |-> " << seq(names) << ", drafts |-> " << function(drafts) << ", kept |-> " << function(strip.kept())
      << ", selected |-> " << str(selected) << ", pending |-> " << (strip.pending() ? "TRUE" : "FALSE") << "]";
    return o.str();
}

std::string record(const char* act, const std::vector<std::string>& list, bool exact, const std::string& name,
        const std::string& text, const Model& model, const AIStrip& strip) {
    std::ostringstream o;
    o << "[act |-> " << str(act) << ", list |-> " << seq(list) << ", exact |-> " << (exact ? "TRUE" : "FALSE")
      << ", name |-> " << str(name) << ", text |-> " << str(text) << ", code |-> " << codeOf(model, strip) << "]";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    bool forget = false;
    bool ignoreSaved = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--faulty-forget") == 0) {
            forget = true;
        } else if (std::strcmp(argv[i], "--faulty-wait") == 0) {
            ignoreSaved = true;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() != 4) {
        std::fprintf(stderr, "usage: strip_walk <seed> <steps> <ais 2-3> [--faulty-forget | --faulty-wait] "
            "<out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(args[0], nullptr, 10);
    const long steps = std::strtol(args[1], nullptr, 10);
    const std::size_t count = std::size_t(std::strtoul(args[2], nullptr, 10));
    if (steps < 1 || count < 2 || count > kAllNames.size()) {
        std::fprintf(stderr, "strip_walk: steps must be at least 1, and AIs 2 or 3\n");
        return 2;
    }
    const std::vector<std::string> names(kAllNames.begin(), kAllNames.begin() + long(count));
    // The saved order names the AIs otherwise than the lists tend to, so that it shows.
    const std::vector<std::string> order = count == 2 ? std::vector<std::string>{"b", "a"}
                                                      : std::vector<std::string>{"c", "a"};
    std::vector<std::string> pool = names;
    pool.push_back(kBad);
    Rng rng{seed};

    // The start: a saved AI (or none), a strip with the sprint tab, nothing kept.
    std::string saved = rng.below(2) == 0 ? std::string() : names[rng.below(names.size())];
    auto model = std::make_unique<Model>(std::vector<nexus::AIInfo>{}, true);
    auto strip = std::make_unique<AIStrip>(order, saved, KeptDrafts{});
    const std::string firstSaved = saved;
    std::vector<std::string> trace{record("start", {}, false, "", "", *model, *strip)};
    Sessions last;
    bool haveLast = false;
    SessionId nextId = 1;
    int lists = 0, removals = 0, restores = 0, restarts = 0;
    for (long k = 0; k < steps; ++k) {
        const std::size_t roll = rng.below(20);
        if (roll < 9) {
            // A list: up to three distinct names, the bad one among them now and then.
            std::vector<std::string> from = pool;
            std::vector<std::string> list;
            const std::size_t n = rng.below(4);
            for (std::size_t i = 0; i < n && !from.empty(); ++i) {
                const std::size_t at = rng.below(from.size());
                list.push_back(from[at]);
                from.erase(from.begin() + long(at));
            }
            const bool exact = rng.below(3) != 0;
            Sessions state;
            if (exact) {
                std::vector<ServerAI> ais;
                for (const std::string& name : list) {
                    ais.push_back(ServerAI{name});
                }
                state.setAIs(std::move(ais));
            } else {
                std::vector<SessionRef> sessions;
                for (const std::string& name : list) {
                    SessionInfo info;
                    info.aiName = name;
                    sessions.push_back(std::make_shared<const SessionView>(tailOf(nextId++, info, {}, {}, Tail{})));
                }
                state = Sessions(std::move(sessions));
            }
            const StripTake t = strip->take(*model, state);
            removals += int(t.removed.size());
            restores += t.keptChanged ? 1 : 0;
            ++lists;
            last = state;
            haveLast = true;
            trace.push_back(record("list", list, exact, "", "", *model, *strip));
        } else if (roll < 11 && haveLast) {
            // The same state again, as a copy: nothing to take.
            const Sessions again = last;
            strip->take(*model, again);
            trace.push_back(record("same", {}, false, "", "", *model, *strip));
        } else if (roll < 15) {
            std::vector<std::size_t> ais;
            for (std::size_t i = 0; i < model->tab_count(); ++i) {
                if (model->tab(i).kind == TabKind::AI) {
                    ais.push_back(i);
                }
            }
            if (ais.empty()) {
                continue;
            }
            const std::size_t i = ais[rng.below(ais.size())];
            const std::string text = kTexts[rng.below(kTexts.size())];
            model->set_draft(i, text);
            trace.push_back(record("type", {}, false, model->tab(i).name, text, *model, *strip));
        } else if (roll < 18) {
            const std::size_t i = rng.below(model->tab_count());
            model->select_tab(i);
            strip->personSelected();
            const nexus::Tab& tab = model->tab(i);
            trace.push_back(record("select", {}, false, tab.kind == TabKind::AI ? tab.name : std::string(), "",
                *model, *strip));
        } else {
            // A quit and a start: the selection saved unless the saved tab still waited, the kept drafts read back.
            if (!strip->pending()) {
                const nexus::Tab& active = model->tab(model->active());
                saved = active.kind == TabKind::AI ? active.name : std::string();
            }
            KeptDrafts kept = forget ? KeptDrafts{} : strip->kept();
            model = std::make_unique<Model>(std::vector<nexus::AIInfo>{}, true);
            strip = std::make_unique<AIStrip>(order, ignoreSaved ? std::string() : saved, std::move(kept));
            haveLast = false;
            ++restarts;
            trace.push_back(record("restart", {}, false, "", "", *model, *strip));
        }
    }
    std::ofstream out(args[3]);
    if (!out) {
        std::fprintf(stderr, "strip_walk: cannot write %s\n", args[3]);
        return 2;
    }
    out << "---- MODULE AIStripTraceData ----\n";
    out << "\\* Written by tests/model/strip_walk: seed " << seed << ", " << steps << " steps, " << count << " AIs"
        << (forget ? ", faulty-forget" : "") << (ignoreSaved ? ", faulty-wait" : "") << ".\n";
    out << "EXTENDS TLC\n";
    out << "TraceNames == {";
    for (std::size_t i = 0; i < names.size(); ++i) {
        out << (i != 0 ? ", " : "") << str(names[i]);
    }
    out << "}\nTraceBad == {" << str(kBad) << "}\nTraceTexts == {";
    for (std::size_t i = 0; i < kTexts.size(); ++i) {
        out << (i != 0 ? ", " : "") << str(kTexts[i]);
    }
    out << "}\nTraceOrder == " << seq(order) << "\nTraceSaved == " << str(firstSaved) << "\nTrace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i) {
        out << "  " << trace[i] << (i + 1 < trace.size() ? ",\n" : "\n");
    }
    out << ">>\n====\n";
    std::printf("strip_walk: %zu records, %d lists, %d tabs removed, %d takes that kept or gave back a draft, "
        "%d restarts\n", trace.size(), lists, removals, restores, restarts);
    return 0;
}
