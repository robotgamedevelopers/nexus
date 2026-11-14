// Writes main_store.h, the store fixtures of a nexus-server from before framed formats (main at 50565c6): two records
// of format 2 and a snapshot of format 3, each the bytes that build's store wrote, so that store_test checks this build
// reads them, not bytes this one claims that build would write. Not part of the build: it compiles against that
// tree's sources only.
//
//   git worktree add <tree> 50565c6
//   c++ -std=c++20 -I<tree>/src tests/store/fixtures/main_store_fixture.cpp <tree>/src/store/store.cpp \
//       <tree>/src/store/journal.cpp <tree>/src/store/codec.cpp <tree>/src/store/mem_fs.cpp -o main_store_fixture
//   ./main_store_fixture > tests/store/fixtures/main_store.h
//
// The steps (store_test's formats group checks what they leave): session 1 made for ada with every setting, a
// prompt, a reply, a notice, a queued prompt and its driver, by client w1's action 1; then the queued prompt taken, by
// its action 2. The snapshot is of the same two steps, compacted.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "store/fs.h"
#include "store/journal.h"
#include "store/store.h"

using namespace nexus::store;

namespace {

Change change(Change::Kind kind) {
    Change c;
    c.kind = kind;
    return c;
}

Change append(std::uint8_t kind, const std::string& text) {
    Change c = change(Change::Kind::Append);
    c.entry = Entry{kind, text, true};
    return c;
}

bool steps(Store& s) {
    Change info = change(Change::Kind::SetInfo);
    info.info = Info{"ada", "claude", "work", "opus", "high"};
    Change queued = change(Change::Kind::Enqueue);
    queued.prompt = "after that, the PR";
    Change driver = change(Change::Kind::SetDriver);
    driver.driver = Driver{2, 0, "s-1", {}};
    const std::vector<Change> first = {info, append(0, "Why did the nightly go red?"), append(1, "Looking at the log."),
        append(2, "Stopped."), queued, driver};
    const std::vector<Change> second = {change(Change::Kind::Dequeue)};
    return s.commit(1, first, "w1", 1).is_ok() && s.commit(1, second, "w1", 2).is_ok();
}

void print(const char* name, const Bytes& bytes) {
    std::printf("const std::uint8_t %s[] = {", name);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::printf("%s%s0x%02x", i == 0 ? "" : ",", i % 16 == 0 ? "\n    " : " ", bytes[i]);
    }
    std::printf("\n};\n");
}

} // namespace

int main() {
    MemFs logs;
    MemFs compacted;
    {
        std::unique_ptr<Store> s;
        if (!Store::open(logs, "store", StoreOptions{}, s).is_ok() || !steps(*s)) {
            return 1;
        }
        StoreOptions small;
        small.compact_bytes = 1;
        std::unique_ptr<Store> c;
        if (!Store::open(compacted, "store", small, c).is_ok() || !steps(*c)) {
            return 1;
        }
    }
    Journal j(logs, "store");
    Journal::Recovery rec;
    Journal k(compacted, "store");
    Journal::Recovery snap;
    if (!j.open(rec).is_ok() || rec.records.size() != 2 || !k.open(snap).is_ok() || snap.snapshot.empty()) {
        return 1;
    }
    std::printf("// Written by main_store_fixture.cpp from main at 50565c6: do not edit.\n#pragma once\n\n"
                "#include <cstdint>\n\n");
    print("kMainRecord2First", rec.records[0]);
    print("kMainRecord2Second", rec.records[1]);
    print("kMainSnapshot3", snap.snapshot);
    return 0;
}
