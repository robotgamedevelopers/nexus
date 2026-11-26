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

#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::uint32_t accountEmailEnd = 0;
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
    std::uint32_t activeAgents = 0;
    std::optional<std::vector<std::string>> busSenders{};
};

// A sessions diff: every session of the state, in id order, then the open secret requests, then the server's AIs
// when this diff carries them (aisSent: they changed from the state diffed from), whether it holds a list
// (aisKnown) and the list, and then the live sign-in flows as this link's window sees them (format 5 on; none in
// formats 1 to 4).
struct SessionsChange {
    std::vector<SessionChange> sessions;
    std::vector<SecretRequest> requests;
    bool aisSent = false;
    bool aisKnown = false;
    std::vector<ServerAI> ais;
    std::vector<SignInOffer> offers;
    std::uint64_t nextAction = 0;
};

// Formats 1 to 3. A format that lacks a field does not write it (activity before 2, the instance before 2, the
// requests before 3, the AIs in every one of them: read, aisKnown is false), and writes a Tool entry as AI
// in format 1. Reading throws BadDiff on malformed bytes.
std::string writeLegacy(const SessionsChange& change, std::uint32_t format);
SessionsChange readLegacy(const std::string& diff, std::uint32_t format);
std::string writeActionsLegacy(std::uint64_t start, const std::vector<Action>& held, std::size_t from,
                               std::uint32_t format);
std::vector<Action> readActionsLegacy(const std::string& diff, std::uint32_t format);

// The most one diff can be: mosh's link carries a diff inside one instruction, which its receiver decompresses into a
// buffer of Compressor::BUFFER_SIZE bytes, 2048 * 2048 (mosh's src/network/compressor.h), and refuses past it. A diff
// longer than that never crossed the link, so a format 4 read refuses it before measuring it.
constexpr std::size_t kMaxDiffBytes = 2048 * 2048;

// The most one format 4 read allocates for the loaded tables (the generated LoadMeasure's region), four times the
// longest diff: 16 MiB. Measured, the region of a real diff is 1.05 times its bytes for a large sessions diff (280,752
// for 267,496) and up to 2.1 times for a small one (840 for 404, 2,344 for 1,863 of actions); bytes crafted as many
// tiny elements measure 32 to 37 times. Four times keeps every real diff of any length the link carries, and bounds a
// crafted one at 16 MiB rather than about 150. A read past it throws BadDiff before it allocates.
constexpr std::size_t kMaxReadBytes = 4 * kMaxDiffBytes;

// The two refusals of those caps, as readTables and readActionsTables make them: BadDiff when a diff of `diffBytes` is
// longer than kMaxDiffBytes, and when its region of `need` bytes is past kMaxReadBytes. `what` names the diff.
void checkDiffSize(std::size_t diffBytes, const char* what);
void checkReadSize(std::size_t diffBytes, std::int64_t need, const char* what);

// Format 4 on: schema's tables. Reading throws BadDiff when the bytes do not load (damaged, cut short, or refused),
// when the diff is longer than kMaxDiffBytes, or when loading it would allocate more than kMaxReadBytes;
// what a later version adds is skipped and counted in `unknown` (fields, enum values: report.unknown), and an entry kind
// this build does not know reads as a notice, an activity as busy and an action kind as Unknown.
//
// The sign-in (format 5 on, sync/signin.h) rides on these tables only on a link of 5 or later (`format`): below it,
// writeTables leaves the flows out and writeActionsTables writes a sign-in's action as a stop naming session 0 and
// nothing else (which every server refuses), and readActionsTables reads a sign-in's kind as Unknown. A flow's URL is
// written only from the offer of the window that holds it (Hub::stateFor), the one route a sign-in secret has into a
// table down; a callback's query, the one route up, rides in an action's `secret` as an answer's value does.
// A complete picker roster can share a snapshot with transcripts, requests and offers. Reserve one
// quarter of both link limits for those other fields. Check the generated representation near the limits.
constexpr std::size_t kRosterWireBytes = kMaxDiffBytes * 3 / 4;
constexpr std::size_t kRosterReadBytes = kMaxReadBytes * 3 / 4;
bool rosterFits(std::vector<ServerAI> ais);

std::string writeTables(const SessionsChange& change, std::uint32_t format);
SessionsChange readTables(const std::string& diff, int* unknown);
std::string writeActionsTables(std::uint64_t start, const std::vector<Action>& held, std::size_t from,
                               std::uint32_t format);
std::vector<Action> readActionsTables(const std::string& diff, std::uint32_t format, int* unknown);

// True when `text` is text schema's tables carry as it is: well-formed UTF-8 with no zero byte. Other text is written
// with each byte that breaks the rule as U+FFFD, so a diff never sends an offset into text whose bytes the window
// holds differently (Sessions::diff_from sends such an entry whole).
bool tableText(std::string_view text);

} // namespace nexus::sync::codec
