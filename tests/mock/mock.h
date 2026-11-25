// The mock: a real nexus-server with fake AIs and fake chats, for driving every client (the Mac window, iPad, iPhone,
// Apple Watch, Windows) in development, screenshots and tests without touching anyone's real AIs, accounts or
// sign-ins (docs/mock.md). This is its pure part: what the mock writes before it starts the server, and what it sends
// to seed each AI's conversation. No files, no time, no processes; tests/mock/nexus_mock.cpp does those.
//
// Each AI's driver is the fake driver (tests/driver/fake_driver.cpp) playing a script of its own over the driver
// protocol (docs/driver-protocol.md): a block for each seeded prompt (`prompt <text>`), answered whenever that prompt
// comes, and replies for any other prompt, taken in turn round and round (`option cycle`). No AI has an account, so
// none needs a sign-in; the sign-in preset adds one AI whose account has no login, only to show that screen.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "accounts/registry.h"

namespace nexus::mock {

// The names the AIs take, in order: pioneers of computing, none of them an AI on anyone's fleet.
const std::vector<std::string>& names();

// The AI the sign-in preset adds, and its account, which has no login.
inline const char* const kSignInName = "Ida";
inline const char* const kSignInAccount = "mock-unsigned";

// The machine the mock's server says it is (its config's `machine`), which every AI's instance names, so that every
// session runs here whatever the host is called.
inline const char* const kMachine = "mock";

// The most AIs the mock makes: the most a server's friends.json holds.
constexpr std::size_t kMaxAis = accounts::kMaxFriends;

// What `nexus-mock start` was asked for.
struct Options {
    std::vector<std::string> ais;   // the AIs' names, in order (empty: the state with no AIs)
    bool signIn = false;            // add kSignInName, whose account needs a sign-in
    bool instant = false;           // replies to new prompts written at once rather than at a reading pace
};

// The first `count` of names(), then mock-<n> for any past them. False, with why, past kMaxAis.
bool aisFor(std::size_t count, std::vector<std::string>* out, std::string* why);

// The names in `list` (comma-separated), each an AI's name (accounts::isName), none twice, none kSignInName. False,
// with why, otherwise.
bool aisFrom(const std::string& list, std::vector<std::string>* out, std::string* why);

// One turn of an AI's seeded conversation.
struct SeedTurn {
    enum class Ends {
        Ok,             // the reply ends by itself
        Stopped,        // the seeder sends a stop once `stopAfter` shows in the reply: the turn ends stopped
        Error,          // the driver reports an error and ends the turn with outcome error
        Cut,            // the driver exits mid-reply: the server ends the turn and says it was cut short
    };
    std::string prompt;             // one line; the key of its block in the AI's script
    Ends ends = Ends::Ok;
    std::string stopAfter{};        // Stopped: the text the seeder waits to see before it sends the stop
    std::string reply{};            // the text of the reply's first block, as the transcript shows it
};

// The seeded conversation of the AI `name`, the `index`th, in the order the seeder sends it: short replies, a long one
// that wraps, markdown with headings and lists, code with long lines, tools, a stopped turn, an error and a turn cut
// by the driver's exit, always ending with an ordinary turn, so that the AI is idle with its driver running when a
// client connects. A client is sent only the newest entries of a transcript (sync::Tail), so the AIs take two orders
// by turns: an even `index` ends with the long reply, the markdown and the code; an odd one with the tools, the stopped
// turn, the error and the cut. Every AI's script answers every seeded prompt, whichever order it takes.
std::vector<SeedTurn> seedFor(const std::string& name, std::size_t index);

// The reply every new prompt to an AI gets first (then the next of the cycle, and so on): the functional test checks it.
std::string firstReplyText();

// The fake driver's script for the AI `name`: hello, its session id, a block for each seeded prompt, and the cycle of
// replies to new prompts, streamed a word at a time at a reading pace unless `instant`.
std::string scriptFor(const std::string& name, bool instant);

// The server's drivers file (server/driver_table.h): one driver per AI, the fake at `fake` with the AI's script at
// `<scripts>/<name>.script`, and the AI working in `<work>/<name>`; the sign-in AI's harness is `claude` (its account's
// provider), its driver the fake all the same.
std::string driversFor(const Options& o, const std::string& fake, const std::string& scripts, const std::string& work);

// The accounts and AIs the server holds (accounts/registry.h): every AI, in order, with no account, not limited (a
// fake runs no tools), on one instance on kMachine in `<work>/<name>`; with the sign-in preset, kSignInName last, linked
// to kSignInAccount (provider claude, no email), which has no login.
accounts::Registry registryFor(const Options& o, const std::string& work);

// The server's config (server/home.h): `machine` kMachine, and with the sign-in preset `claude <claude>`, the FAKE
// claude that in-app sign-in runs, so that a sign-in started in a client never reaches a real one.
std::string serverConfigFor(const Options& o, const std::string& claude);

// The command a client runs on the server's machine instead of `nexus-server new` to reach the mock: the server at
// `server` with HOME the mock's `home` and --dir the mock's `dir`. Words separated by single spaces, as a client's
// --server-command takes them; false, with why, when a path holds white space (the words could not be told apart).
bool serverCommandFor(const std::string& server, const std::string& home, const std::string& dir, std::string* out,
    std::string* why);

// `s` as a JSON string literal.
std::string jsonQuote(const std::string& s);

} // namespace nexus::mock
