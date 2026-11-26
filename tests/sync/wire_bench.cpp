// A measurement, not a test (no ctest entry): how long the sessions diff and the actions take to encode and decode,
// in each state format, so that a change to the wire is weighed against the window's 120 Hz frame (8.3 ms). The
// window decodes on the link thread, not the frame thread; the numbers say what that thread spends.
//
//   wire_bench [rounds]     prints, per message and format, its size and the mean ms of one encode and one decode
//
// The messages:
//   typical   one session among four whose open entry grew by 120 bytes: what a streaming AI sends each frame
//   large     sixteen sessions of sixteen 1 KiB entries each (the tail's count), from an empty state: a window's first
//             state, about the largest diff the link carries
//   actions   eight sends of 200 bytes each, from an empty state

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "sync/actions.h"
#include "sync/formats.h"
#include "sync/sessions.h"

namespace {

using namespace nexus::sync;
using Clock = std::chrono::steady_clock;

EntryRef entry(nexus::EntryKind kind, std::string text, bool closed) {
    return std::make_shared<const SessionEntry>(SessionEntry{kind, std::move(text), closed});
}

SessionRef session(SessionId id, std::size_t entries, std::size_t bytes, std::size_t openBytes) {
    auto v = std::make_shared<SessionView>();
    v->id = id;
    v->info.aiName = "ai-" + std::to_string(id);
    v->info.harness = "claude";
    v->info.instance = "studio";
    v->activity = Activity::Busy;
    for (std::size_t i = 0; i < entries; ++i) {
        const bool last = i + 1 == entries;
        const std::size_t n = last && openBytes != 0 ? openBytes : bytes;
        v->entries.push_back(entry(i % 2 == 0 ? nexus::EntryKind::Person : nexus::EntryKind::AI,
            std::string(n, static_cast<char>('a' + i % 26)), !(last && openBytes != 0)));
    }
    v->queue = {"a queued prompt"};
    return v;
}

double ms(Clock::duration d, int rounds) {
    return std::chrono::duration<double, std::milli>(d).count() / rounds;
}

void sessionsCase(const char* name, const Sessions& from, const Sessions& to, std::uint32_t format, int rounds) {
    Sessions a = from;
    Sessions b = to;
    a.setFormat(format);
    b.setFormat(format);
    std::string diff;
    const Clock::time_point e0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        diff = b.diff_from(a);
    }
    const Clock::time_point e1 = Clock::now();
    const Clock::time_point d0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        Sessions c = a;
        c.apply_string(diff);
        if (!(c == b)) {
            std::fprintf(stderr, "wire_bench: %s format %u did not apply to the state it came from\n", name, format);
            std::exit(1);
        }
    }
    const Clock::time_point d1 = Clock::now();
    std::printf("%-8s format %u  %8zu bytes  encode %.4f ms  decode %.4f ms\n", name, format, diff.size(),
        ms(e1 - e0, rounds), ms(d1 - d0, rounds));
}

void actionsCase(std::uint32_t format, int rounds) {
    Actions a(1);
    a.setFormat(format);
    for (int i = 0; i < 8; ++i) {
        Action x;
        x.kind = ActionKind::Send;
        x.session = 1;
        x.text = std::string(200, 'p');
        a.add(x);
    }
    const Actions empty(1);
    std::string diff;
    const Clock::time_point e0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        diff = a.diff_from(empty);
    }
    const Clock::time_point e1 = Clock::now();
    std::size_t read = 0;
    const Clock::time_point d0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        read += Actions::parse(diff, format).size();
    }
    const Clock::time_point d1 = Clock::now();
    if (read != static_cast<std::size_t>(rounds) * 8) {
        std::fprintf(stderr, "wire_bench: actions format %u read back short\n", format);
        std::exit(1);
    }
    std::printf("%-8s format %u  %8zu bytes  encode %.4f ms  decode %.4f ms\n", "actions", format, diff.size(),
        ms(e1 - e0, rounds), ms(d1 - d0, rounds));
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 2000;
    if (rounds <= 0) {
        std::fprintf(stderr, "usage: wire_bench [rounds]\n");
        return 2;
    }
    // typical: four sessions of sixteen 400-byte entries; the first's open entry grows from 300 to 420 bytes.
    std::vector<SessionRef> before;
    std::vector<SessionRef> after;
    for (SessionId id = 1; id <= 4; ++id) {
        before.push_back(session(id, 16, 400, id == 1 ? 300 : 0));
        after.push_back(id == 1 ? session(id, 16, 400, 420) : before.back());
    }
    const Sessions typicalFrom(before);
    const Sessions typicalTo(after);
    std::vector<SessionRef> large;
    for (SessionId id = 1; id <= 16; ++id) {
        large.push_back(session(id, 16, 1024, 0));
    }
    const Sessions empty;
    const Sessions largeTo(large);
    for (std::uint32_t format : {kTablesFormat, kSecretsFormat}) {
        sessionsCase("typical", typicalFrom, typicalTo, format, rounds);
        sessionsCase("large", empty, largeTo, format, rounds / 20 + 1);
        actionsCase(format, rounds);
    }
    return 0;
}
