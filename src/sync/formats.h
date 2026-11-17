// The versions of what the window and nexus-server send each other, and how a window and a server of different
// versions agree on one (docs/formats.md says the whole scheme). A window and a server never have to be the same
// version: each reads every older format, a format from 2 on frames each session and entry so that a reader skips the
// fields it does not know, and a link uses the newer format both ends read.

#pragma once

#include <cstdint>
#include <string>

namespace nexus::sync {

// The sessions state (sync/sessions.h), down from the server to the window, and the actions (sync/actions.h), up.
//   1  the first: no activity, entry kinds Person, Friend and Notice; nothing framed, so a reader of 1 reads exactly 1.
//      A server that states no format (one before formats were stated) sends 1, and a window that states none reads 1.
//   2  each changed session and each entry framed by its length, the session's activity after its settings, and entry
//      kind Tool. A reader of 2 reads any later format, skipping what it does not know.
//   3  the open secret requests after the sessions (sync/secrets.h), each framed by its length; and the actions, up,
//      framed too (sync/actions.h), with the secret dialog's answer and cancel. A window of format 2 receives no
//      requests, and the server counts it as unable to take one.
//   4  the state and the actions as schema's variable tables (schema/Nexus.schema: SessionsDiff and ActionsDiff), where
//      every field, enum value and list rides under the hash of its name: a reader skips and counts what it does not
//      know, reads what an older writer left out as its declared default, and reads an enum value it does not know as
//      None (an entry kind as a notice, an activity as busy, an action kind as unknown). From 4 a later version adds
//      fields and values in the .schema file and does not raise the format; a new format number is for a change an
//      older reader cannot read through. Formats 1 to 3 are the hand-written codecs, kept for windows and servers from
//      before 4 and never extended.
//   5  format 4's tables with the in-app sign-in (sync/signin.h): SessionsDiff.signins, the live flows as each window
//      sees them, and the action kinds SignInClaim, SignInOpened and SignInCallback. A format 4 reader reads through
//      them (the field skipped, the kinds unknown and refused alone) but would never act on a flow, so the server
//      must know which windows do, and that is the link's format, from the Hello: it writes the flows and takes the
//      sign-in actions only on a link of 5 or later, and while no linked window is, it asks for a login by a paste.
constexpr std::uint32_t kStateFormat = 5;

// The first format that carries secret requests down and secret answers up.
constexpr std::uint32_t kSecretsFormat = 3;

// The first format written as schema's tables.
constexpr std::uint32_t kTablesFormat = 4;

// The first format that carries sign-ins down and the sign-in actions up.
constexpr std::uint32_t kSignInFormat = 5;

// The oldest format of the other end this build still works with: its Hello's min_peer. A window refuses a server
// below it, and a server a window below it, each with one plain line. Every format is, today.
constexpr std::uint32_t kMinPeer = 1;

// What each end says first on a link (schema/Nexus.schema table Hello): the format it speaks (proto) and the oldest it
// works with (minPeer). An end refuses the link only when the other's proto is below its own minPeer; otherwise the
// link speaks the older of the two protos.
struct Hello {
    std::uint32_t proto = 0;
    std::uint32_t minPeer = 0;
    bool operator==(const Hello&) const = default;
};

// This build's Hello.
constexpr Hello kHello{kStateFormat, kMinPeer};

// The Hello of an end that sent none but stated a format, as an end from before Hello did: it speaks that format (0:
// it stated none, so 1) and works with every older one.
inline Hello statedFormat(std::uint32_t format) {
    return Hello{format == 0 ? 1 : format, 1};
}

// A Hello as schema's variable table, in lowercase hex, which goes in one word of a line (link/bootstrap.h).
std::string helloText(const Hello& hello);
// The Hello `text` holds. False, with why in one line, when it is not lowercase hex of a Hello that loads, or its
// proto is 0. `unknown` (when given) counts the fields of a later version's Hello this build skipped.
bool readHello(const std::string& text, Hello* out, std::string* why, int* unknown = nullptr);

// How the window states its Hello and its format to `nexus-server new` over ssh: environment variables, which a
// nexus-server from before them ignores (an argument it would refuse). A nexus-server from before Hello reads only the
// format.
constexpr const char* kStateFormatVariable = "NEXUS_STATE_FORMAT";
constexpr const char* kHelloVariable = "NEXUS_HELLO";

// The format a link uses: the newer of the two that both ends read, so the older of the two stated.
inline std::uint32_t linkFormat(std::uint32_t window, std::uint32_t server) {
    return window < server ? window : server;
}

// Whether a window on a link of `format` (linkFormat of the two Hellos' protos) acts on the in-app sign-in: the one way
// the server knows it, with no second capability mechanism. With no such window linked, a login is asked for by a
// paste (sync/hub.h SignInHooks::takers).
inline bool takesSignIn(std::uint32_t format) {
    return format >= kSignInFormat;
}

// Empty when a window stating `window` can be served by a server whose floor is `oldest`; otherwise the one plain
// line the window shows (the server also logs it).
std::string windowRefusal(std::uint32_t window, std::uint32_t oldest = kMinPeer);
// Empty when a window can read a server answering with `server`, given the window's floor `oldest`; otherwise the one
// plain line the window shows.
std::string serverRefusal(std::uint32_t server, std::uint32_t oldest = kMinPeer);

// The server's decision on a window's Hello, against its own: empty when the link may be made, else the one plain line
// the window shows (the server logs it too). It refuses a window whose proto is below the server's minPeer, and, so
// that the window hears it at once rather than from its own check, a server whose proto is below the window's minPeer.
std::string refusalOfWindow(const Hello& window, const Hello& server = kHello);
// The window's decision on the server's Hello, against its own: empty, or the one plain line it shows.
std::string refusalOfServer(const Hello& server, const Hello& window = kHello);

} // namespace nexus::sync
