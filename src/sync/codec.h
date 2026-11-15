// The bytes of the state and the actions, apart from what they mean. Sessions::diff_from and Actions::diff_from work
// out what a diff says (which sessions changed, which entries, from which offset); a codec only writes that to bytes and
// reads it back, in the link's format (sync/formats.h). Sessions::apply_string and Actions::apply_string then check what
// was read against the state it is applied to.
//
//   format 4 on   schema's variable tables, generated from schema/Nexus.schema (sync/tables_codec.cpp)
//   formats 1-3   the hand-written codecs from before schema (sync/legacy_codec.cpp, sync/wire.h), kept for windows and
//                 servers from before format 4 and never extended
//
// Internal to src/sync.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sync/actions.h"
#include "sync/sessions.h"
#include "sync/wire.h"

namespace nexus::sync::codec {

// One entry of a changed session: its kind, whether it is closed, how many bytes of its text the state diffed from
// already holds (non-zero only for an entry that state holds open and that only grew), and the rest of its text.
struct EntryChange {
    EntryKind kind = EntryKind::Notice;
    bool closed = true;
    std::uint64_t offset = 0;
    std::string tail;
};

// One session of a diff. Unchanged: the state diffed from holds it as it is, and only its id is set. Changed: what it
// is, its activity, its first transcript index, the index of the first entry the state diffed from does not hold as it
// is (keepFrom), the entries from there to the end, and its queue when queueChanged.
struct SessionChange {
    SessionId id = 0;
    bool changed = false;
    SessionInfo info;
    Activity activity = Activity::Idle;
    std::uint64_t first = 0;
    std::uint64_t keepFrom = 0;
    std::vector<EntryChange> entries;
    bool queueChanged = false;
    std::vector<std::string> queue;
};

// A sessions diff: every session of the state, in id order, then the open secret requests.
struct SessionsChange {
    std::vector<SessionChange> sessions;
    std::vector<SecretRequest> requests;
};

// Formats 1 to 3. A format that lacks a field does not write it (activity before 2, the instance before 2, the
// requests before 3), and writes a Tool entry as Friend in format 1. Reading throws BadDiff on malformed bytes.
std::string writeLegacy(const SessionsChange& change, std::uint32_t format);
SessionsChange readLegacy(const std::string& diff, std::uint32_t format);
std::string writeActionsLegacy(std::uint64_t start, const std::vector<Action>& held, std::size_t from,
                               std::uint32_t format);
std::vector<Action> readActionsLegacy(const std::string& diff, std::uint32_t format);

// Format 4 on: schema's tables. Reading throws BadDiff when the bytes do not load (damaged, cut short, or refused);
// what a later version adds is skipped and counted in `unknown` (fields, enum values: report.unknown), and an entry kind
// this build does not know reads as a notice, an activity as busy and an action kind as Unknown.
std::string writeTables(const SessionsChange& change);
SessionsChange readTables(const std::string& diff, int* unknown);
std::string writeActionsTables(std::uint64_t start, const std::vector<Action>& held, std::size_t from);
std::vector<Action> readActionsTables(const std::string& diff, int* unknown);

// True when `text` is text schema's tables carry as it is: well-formed UTF-8 with no zero byte. Other text is written
// with each byte that breaks the rule as U+FFFD, so a diff never sends an offset into text whose bytes the window
// holds differently (Sessions::diff_from sends such an entry whole).
bool tableText(std::string_view text);

} // namespace nexus::sync::codec
