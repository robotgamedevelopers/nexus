// The relay's walk (watch/core/relay.h), for RelayTrace.tla: a seeded random walk through the real WatchLink and
// PhoneRelay, with a carrier that loses, repeats and reorders frames and answers, a link the walk plays (it delivers
// what a run handed, in any order, and acknowledges all once all are delivered), and phone restarts (a new PhoneRelay
// from the old one's persisted state; what the old link held may still be delivered). Each event and the state after
// it is written as a TLA+ module TLC checks is a behaviour of Relay.tla. With a fault (NEXUS_WATCH_FAULTS) the check
// must reject the trace.
//
//   relay_walk <seed> <steps> <out RelayTraceData.tla> [forget-handed | ack-early | vouch-all]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "relay.h"

using namespace nexus::watch;

namespace {

constexpr std::uint64_t kN = 5;         // prompts the watch makes
constexpr std::uint64_t kMaxCrash = 2;
constexpr std::size_t kMaxMsgs = 3;

template <class C>
std::string set(const C& c) {
    std::ostringstream o;
    o << "{";
    bool first = true;
    for (std::uint64_t n : c) {
        o << (first ? "" : ", ") << n;
        first = false;
    }
    o << "}";
    return o.str();
}

template <class C>
std::string seq(const C& c) {
    std::ostringstream o;
    o << "<<";
    bool first = true;
    for (std::uint64_t n : c) {
        o << (first ? "" : ", ") << n;
        first = false;
    }
    o << ">>";
    return o.str();
}

using Nums = std::set<std::uint64_t>;
using Answer = std::pair<std::uint64_t, std::uint64_t>;   // uncertain, applied

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: relay_walk <seed> <steps> <out.tla> [fault]\n");
        return 2;
    }
    const unsigned long seed = std::strtoul(argv[1], nullptr, 10);
    const long steps = std::strtol(argv[2], nullptr, 10);
    if (argc == 5) {
#ifdef NEXUS_WATCH_FAULTS
        gWatchFault = argv[4];
#else
        std::fprintf(stderr, "relay_walk: built without NEXUS_WATCH_FAULTS: no fault %s\n", argv[4]);
        return 2;
#endif
    }
    std::mt19937_64 rng(seed);
    const auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };

    WatchLink watch{"w"};
    std::uint64_t epoch = 1;
    auto relay = std::make_unique<PhoneRelay>(PhoneRelay::Persisted{"w", 0}, epoch);
    relay->setUnlocked(true);   // the phone's lock passed (its refusal while locked is unit tested)
    std::uint64_t made = 0, crashes = 0;
    Nums told, dropped;
    std::map<Nums, WatchFrame> reqs;
    std::map<Answer, PhoneFrame> resps;
    Nums linkQ;
    Nums runHanded;                     // this run's handed actions: the link acknowledges once all are delivered
    std::uint64_t linkSent = 0;         // this run's link's count
    std::vector<std::uint64_t> applied(kN + 1, 0);

    const auto outbox = [&] {
        Nums o;
        for (const Prompt& p : watch.outbox())
            o.insert(p.num);
        return o;
    };
    const auto take = [&](const PhoneFrame& f) {
        const Nums before = outbox();
        const WatchLink::Taken t = watch.take(f);
        Nums doubt;
        for (const Prompt& p : t.lost)
            doubt.insert(p.num);
        const Nums after = outbox();
        for (std::uint64_t n : before) {
            if (doubt.count(n))
                told.insert(n);
            else if (!after.count(n))
                dropped.insert(n);
        }
    };
    const auto view = [&](bool acknowledged) {
        LinkView v;
        v.phase = LinkPhase::Connected;
        v.aisKnown = true;
        v.ais = {"a"};
        LinkSession s;
        s.id = 1;
        s.aiName = "a";
        s.activity = LinkSession::Activity::Busy;
        v.sessions = {s};
        v.sent = linkSent;
        v.acknowledged = acknowledged;
        return v;
    };
    const auto state = [&] {
        std::ostringstream o;
        o << "[made |-> " << made << ", outbox |-> " << set(outbox()) << ", told |-> " << set(told)
          << ", dropped |-> " << set(dropped) << ", reqs |-> {";
        bool first = true;
        for (const auto& [r, f] : reqs) {
            o << (first ? "" : ", ") << set(r);
            first = false;
        }
        o << "}, resps |-> {";
        first = true;
        for (const auto& [m, f] : resps) {
            o << (first ? "" : ", ") << "[uncertain |-> " << m.first << ", applied |-> " << m.second << "]";
            first = false;
        }
        std::vector<std::uint64_t> waiting, inflight;
        for (const Prompt& p : relay->waiting())
            waiting.push_back(p.num);
        for (const auto& [n, at] : relay->inflight())
            inflight.push_back(n);
        o << "}, handed |-> " << relay->persisted().handed << ", boot |-> " << relay->bootHanded()
          << ", acked |-> " << relay->acked() << ", waiting |-> " << seq(waiting) << ", inflight |-> " << seq(inflight)
          << ", linkQ |-> " << set(linkQ) << ", applied |-> "
          << seq(std::vector<std::uint64_t>(applied.begin() + 1, applied.end())) << ", crashes |-> " << crashes << "]";
        return o.str();
    };

    std::vector<std::string> trace{"[act |-> \"init\", r |-> {}, m |-> [uncertain |-> 0, applied |-> 0], n |-> 0, s |-> " +
                                   state() + "]"};
    for (long k = 0; k < steps; ++k) {
        std::string act;
        Nums r;
        Answer m{0, 0};
        std::uint64_t n = 0;
        const std::size_t e = pick(12);
        if (e == 0 && made < kN) {
            act = "make";
            ++made;
            watch.add(Prompt{made, "a", "p" + std::to_string(made)});
        } else if (e == 1 && reqs.size() < kMaxMsgs) {
            act = "send";
            r = outbox();
            reqs[r] = watch.frame();
        } else if (e == 2 && !reqs.empty()) {
            act = "receive";
            auto it = std::next(reqs.begin(), static_cast<long>(pick(reqs.size())));
            r = it->first;
            const PhoneFrame f = relay->exchange(it->second).reply;
            if (resps.size() < kMaxMsgs)
                resps.emplace(Answer{f.uncertain, f.applied}, f);
        } else if (e == 3 && !relay->waiting().empty()) {
            act = "hand";
            for (const LinkAction& a : relay->link(view(false))) {
                ++linkSent;
                linkQ.insert(a.prompt);
                runHanded.insert(a.prompt);
            }
        } else if (e == 4 && !linkQ.empty()) {
            act = "deliver";
            n = *std::next(linkQ.begin(), static_cast<long>(pick(linkQ.size())));
            linkQ.erase(n);
            ++applied[n];
        } else if (e == 5 && !relay->inflight().empty() && relay->waiting().empty() &&
                   std::none_of(runHanded.begin(), runHanded.end(), [&](std::uint64_t x) { return linkQ.count(x); })) {
            act = "ack";
            relay->link(view(true));
        } else if (e == 6 && !resps.empty()) {
            act = "take";
            auto it = std::next(resps.begin(), static_cast<long>(pick(resps.size())));
            m = it->first;
            take(it->second);
        } else if (e == 7) {
            act = "roundtrip";
            const PhoneFrame f = relay->exchange(watch.frame()).reply;
            take(f);
        } else if (e == 8 && crashes < kMaxCrash && pick(4) == 0) {
            act = "crash";
            ++crashes;
            relay = std::make_unique<PhoneRelay>(relay->persisted(), ++epoch);
            relay->setUnlocked(true);
            runHanded.clear();
            linkSent = 0;
        } else if (e == 9 && !reqs.empty()) {
            act = "losereq";
            auto it = std::next(reqs.begin(), static_cast<long>(pick(reqs.size())));
            r = it->first;
            reqs.erase(it);
        } else if (e == 10 && !resps.empty()) {
            act = "loseresp";
            auto it = std::next(resps.begin(), static_cast<long>(pick(resps.size())));
            m = it->first;
            resps.erase(it);
        } else {
            continue;
        }
        trace.push_back("[act |-> \"" + act + "\", r |-> " + set(r) + ", m |-> [uncertain |-> " +
                        std::to_string(m.first) + ", applied |-> " + std::to_string(m.second) + "], n |-> " +
                        std::to_string(n) + ", s |-> " + state() + "]");
    }

    std::ofstream out(argv[3]);
    out << "---------------------------- MODULE RelayTraceData ----------------------------\n"
        << "\\* Written by relay_walk " << argv[1] << " " << argv[2] << (argc == 5 ? std::string(" ") + argv[4] : "")
        << ": one walk of the real WatchLink and PhoneRelay.\n"
        << "TraceN == " << kN << "\nTraceMaxCrash == " << kMaxCrash << "\nTraceMaxMsgs == " << kMaxMsgs << "\n"
        << "Trace == <<\n";
    for (std::size_t k = 0; k < trace.size(); ++k)
        out << "  " << trace[k] << (k + 1 < trace.size() ? ",\n" : "\n");
    out << ">>\n=============================================================================\n";
    return out ? 0 : 1;
}
