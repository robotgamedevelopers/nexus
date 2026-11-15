// The two versions of nexus's formats the cross-version tests exchange bytes between: this build's (schema/Nexus.schema,
// wire_test.cpp) and a later one's (tests/wire/next/NexusNext.schema, wire_next.cpp). Each version's generated code is
// compiled in its own translation unit, so the two meet only here, through plain bytes and plain values: as a window
// and a server of two versions meet only through the bytes on the link and in the store.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wiretest {

using Bytes = std::vector<std::uint8_t>;

// What a reader of either version made of some bytes: the read's report, then what it read.
struct Read {
    bool loaded = false;
    int unknown = 0;            // fields, enum values and union arms the reader skipped because it does not know them
    bool damaged = false;
    bool refused = false;
    std::string friendName;     // the first session's info
    std::string instance;
    bool titlePresent = false;  // the later version's added field (a reader of this version never sees it)
    std::string title;
    std::vector<std::string> kinds;     // each entry's kind, by name ("None": a kind the reader does not know)
    std::vector<std::string> changes;   // each record change's arm, by name ("None": an arm the reader does not know)
    std::vector<std::string> queue;     // the first session's queue
};

// What a reader of the later version made of an actions diff: the read's report, then each action, in order.
struct ReadActions {
    bool loaded = false;
    int unknown = 0;
    bool damaged = false;
    bool refused = false;
    std::uint64_t start = 0;
    std::vector<std::string> kinds;     // by name
    std::vector<std::uint64_t> sessions;
    std::vector<std::string> friends;
    std::vector<std::string> texts;
    std::vector<std::uint64_t> requests;
    std::vector<std::string> secrets;
};

// What a reader of the later version made of a Hello.
struct ReadHello {
    bool loaded = false;
    int unknown = 0;
    std::uint32_t proto = 0;
    std::uint32_t minPeer = 0;
    bool buildPresent = false;
};

// The later version: a sessions diff, from an empty state, of two sessions: the first with the added field set and
// entries that include the added kind (Image), the second with the added activity (Waiting); a record whose changes
// include the added arm (set_title); and its readers of each.
Bytes nextSessions();
Bytes nextRecord();
Read nextReadSessions(const Bytes& bytes);
Read nextReadRecord(const Bytes& bytes);

// The later version's actions diff, from action 5: a send to session 7 ("hi"), the added kind (Rename, of session 7 to
// "new name") with the added field (note) set, a secret answer (request 3) and a stop of session 7; and its reader.
Bytes nextActions();
ReadActions nextReadActions(const Bytes& bytes);

// The later version's Hello, with the added field (build) set; and its reader.
Bytes nextHello(std::uint32_t proto, std::uint32_t minPeer);
ReadHello nextReadHello(const Bytes& bytes);

} // namespace wiretest
