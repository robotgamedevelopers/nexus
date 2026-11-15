// What a placed build is, as text and as decisions, without the filesystem (server/builds.h does the files): the
// manifest every build folder holds, the hash a build is named by, the evidence that its exact content passed its
// tests, the format versions it reads and writes, the names in the builds directory, and the rules a switch and a
// prune keep (docs/builds.md; models/placement/Placement.tla). Pure: no files, no time.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nexus::server::builds {

// Names under the server's directory (server/home.h): the builds directory, and in it the pointer and each build's
// manifest; the stable link people and service files use; and what a placed tree must hold.
inline const char* const kBuildsDir = "builds";
inline const char* const kActive = "active";            // builds/active -> pair-<current>-<previous|none>
inline const char* const kStable = "current";           // <server dir>/current -> builds/active/current
inline const char* const kManifest = "manifest";
inline const char* const kFormatsFile = "formats";
inline const char* const kProgram = "bin/nexus-server";
// The top-level names a build may hold: the server program (bin/), the driver tree (drivers/), its formats, and the
// window application where it is placed (nexus.app/, on macOS).
inline const std::vector<std::string> kTopLevel{"bin", "drivers", "formats", "nexus.app"};

// The prefixes of what an interrupted run leaves, each made only by the builds lock's holder (so the next holder
// removes it safely), and of a pair.
inline const char* const kTmpPrefix = ".tmp-";      // a build being placed
inline const char* const kPairTmpPrefix = ".pair-"; // a pair being made
inline const char* const kActiveTmpPrefix = ".active-";  // a new pointer link, not yet renamed over `active`
inline const char* const kGonePrefix = ".gone-";    // a build being pruned
inline const char* const kPairPrefix = "pair-";

// One file, directory or symbolic link of a build, by its path inside the build ("bin/nexus-server").
enum class EntryKind { Dir, File, Link };
struct Entry {
    EntryKind kind = EntryKind::File;
    std::string path;
    bool exec = false;          // a file: executable (placed 0555, else 0444)
    std::uint64_t size = 0;     // a file: its size
    std::string sha;            // a file: the SHA-256 of its bytes, lowercase hex
    std::string target;         // a link: what it points to (relative, inside the build)
    bool operator==(const Entry&) const = default;
};

// An entry's manifest line, tab-separated, without its newline: `dir <path>`, `file <x|-> <size> <sha> <path>`,
// `link <target> <path>`.
std::string entryLine(const Entry& e);
// The entries in path order, one line each: the text a build's hash is the SHA-256 of. Everything in the folder but
// the manifest is in it: the program, the driver tree, the formats, the application.
std::string entriesText(std::vector<Entry> entries);
// `leaveOutDrivers` hashes without the driver tree: the trace walk's reversed fault (builds.h Fault::TestedProgramOnly)
// and nothing else.
std::string treeHash(const std::vector<Entry>& entries, bool leaveOutDrivers = false);

// Why `path` cannot be in a build (empty when it can): relative, no empty, `.` or `..` part, no control character or
// tab, at most 1024 bytes, under one of kTopLevel.
std::string pathProblem(const std::string& path);
// Why a link at `path` to `target` cannot be in a build (empty when it can): relative, and inside the build.
std::string linkProblem(const std::string& path, const std::string& target);

// One format a build writes: its version, and the oldest version of a reader that can read what it writes
// (`format <name> <writes> <reader>`). A build reads every version of a format up to the one it writes.
struct Format {
    std::string name;
    std::uint32_t writes = 0;
    std::uint32_t reader = 0;
    bool operator==(const Format&) const = default;
};
using Formats = std::vector<Format>;
const Format* findFormat(const Formats& formats, const std::string& name);
// The formats file (`format <name> <writes> <reader>` lines; `#` comments and blank lines skipped). False, with the
// line and the fix in `why`, for anything else, a name twice, or a reader past its writes.
bool readFormats(const std::string& text, Formats* out, std::string* why);
std::string formatsText(const Formats& formats);
// The formats a switch checks: the store's two (records and snapshots: a build refuses a record or snapshot whose
// reader is past what it writes) and the config files' (accounts.json and friends.json: a newer version is refused).
// The link and the driver protocol are agreed between their two ends at each connection, and not checked.
inline const std::vector<std::string> kCheckedFormats{"store-record", "store-snapshot", "config"};
// The rule a switch keeps (current/back): the build made current must read what the current build writes, for every
// checked format: current's reader <= the target's writes. Empty when it does; else the plain line. Whether the store
// holds anything a build cannot read is layer 5's check; this one goes by the versions alone.
std::string formatsRefusal(const Formats& target, const std::string& targetHash, const Formats& current,
                           const std::string& currentHash);

// The evidence that a tree passed its tests: written by whatever ran them, after they ran, naming the hash of the tree
// as it was then (`nexus-server builds hash <dir>`).
//   nexus-test-evidence 1
//   tree <hash>
//   result pass
//   run <one line: what ran, and its counts>
struct Evidence {
    std::string tree;
    std::string result;
    std::string run;
};
bool readEvidence(const std::string& text, Evidence* out, std::string* why);
std::string evidenceText(const Evidence& e);

// A build's manifest, the file `manifest` in its folder:
//   nexus-build 1
//   tree <hash>                      the folder's name: treeHash of the entries below
//   format <name> <writes> <reader>  as the build's formats file says, one line each
//   tested <hash> pass <run>         the evidence, when the placement had valid evidence for this hash; else absent
//   dir|file|link ...                every entry, in path order (entryLine)
//   end                              a manifest without it is incomplete
struct Manifest {
    std::string tree;
    Formats formats;
    bool tested = false;
    std::string run;            // the evidence's run line, when tested
    std::vector<Entry> entries;
};
std::string manifestText(const Manifest& m);
// False, with why, for a manifest that does not read: an unknown line, a field that does not parse, no `end`, or a
// tree that is not the hash of its entries, or a `tested` line for another hash.
bool readManifest(const std::string& text, Manifest* out, std::string* why, bool leaveOutDrivers = false);

// 64 lowercase hex digits.
bool isHash(const std::string& s);
// What a name in the builds directory is.
enum class NameKind { Build, Pair, Active, Tmp, PairTmp, ActiveTmp, Gone, Other };
NameKind classify(const std::string& name);
// A pair's name: pair-<current>-<previous>, or pair-<current>-none.
std::string pairName(const std::string& current, const std::string& previous);
bool readPairName(const std::string& name, std::string* current, std::string* previous);

// A build and when it was placed (its manifest's modification time, in nanoseconds since the epoch: a fact read from
// the disk, which orders the builds and is used for nothing else).
struct Placed {
    std::string hash;
    std::int64_t placedNs = 0;
};
// The builds a prune removes, oldest first: every build but current, previous and the `keep` most recently placed of
// the rest. A build a running server holds is kept when its turn comes (builds.h), whatever this says.
std::vector<std::string> pruneCandidates(std::vector<Placed> builds, const std::string& current,
                                         const std::string& previous, std::size_t keep);

// The placed build a program at `path` (an absolute, resolved path) is: `<...>/builds/<hash>/bin/nexus-server` gives
// <hash>; anything else (a build tree, a copy) gives "".
std::string buildOfProgram(const std::string& path);

} // namespace nexus::server::builds
