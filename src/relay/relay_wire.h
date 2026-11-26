// The relay: how the native Windows window reaches nexus-server through a link that runs inside WSL2.
//
// On Windows only the window is native. mosh's transport (cmake/mosh.cmake), compiled from its own sources unmodified,
// does not run natively there (its sender reads /dev/urandom for every datagram's chaff, src/crypto/prng.h), so the
// window's link, its thread, bootstrap, redial and mosh session (link/client.h, link/client_port.h), runs as the Linux
// build inside WSL2: the program nexus-link-relay (src/link/relay_main.cpp). The window starts it with wsl.exe and talks
// to it over the process's standard input and output (relay/relay_port.h): a pipe of its own that no other process can
// write to, which needs no port, no key and no network setting, and which ends when either end does.
//
// What crosses the pipe is the link's interface as the window's frame sees it (link_port.h LinkPort), unchanged:
// actions down to the relay, each once and in order, numbered by the relay's link as by the window's own link on macOS
// and Linux; the link's state up, its sessions as diffs of sync::Sessions (the diffs mosh carries), so that the
// window's state after each frame equals the relay's link's. Nothing about the link changes: the relay's ClientPort
// redials, resends and is the window's link in every respect (models/Link.tla, models/Reconnect.tla).
//
// A frame is one byte of kind, its body's length as four bytes little-endian, then the body. Each end first sends a
// Hello (kRelayVersion and the state format it reads, sync::kStateFormat) and reads none of the other's frames until
// it has read the other's Hello and found it its own: a window and a relay of two builds refuse each other in one line
// that says which to update, never a stream that cannot be read. No OS and no time here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "link_port.h"
#include "sync/actions.h"
#include "sync/sessions.h"

namespace nexus::relay {

// The relay protocol's version: raised with any change to the frames below.
constexpr std::uint32_t kRelayVersion = 1;
// The longest body a frame may have; a longer one is refused as the stream gone wrong.
constexpr std::uint32_t kMaxBody = 256u << 20;

enum class FrameKind : std::uint8_t {
    Hello = 1,      // both ways, first: "nexus-relay", the version and the state format
    State = 2,      // relay to window: the link's state (StateWriter)
    Action = 3,     // window to relay: one action for the server
    History = 4,    // window to relay: LinkPort::requestHistory's arguments
};

struct Frame {
    FrameKind kind = FrameKind::Hello;
    std::string body;
};

// A frame's bytes.
std::string frameBytes(FrameKind kind, const std::string& body);

// Cuts a byte stream into frames. Once it fails (a kind it does not know, a body over kMaxBody) it stays failed.
class FrameReader {
public:
    void feed(const char* data, std::size_t size);
    // The next whole frame: true with it; false with nothing (`why` empty: wait for more bytes) or because the stream
    // is not frames (`why` says how).
    bool next(Frame* out, std::string* why);
    bool failed() const { return !failure_.empty(); }

private:
    std::string buffer_;
    std::size_t at_ = 0;
    std::string failure_;
};

// This end's Hello.
std::string helloFrame();
// True for the other end's Hello when it is this end's own; else false and why, naming `self` and `other` (as "this
// window" and "nexus-link-relay in WSL2") and both ends' version and format.
bool checkHello(const std::string& body, const char* self, const char* other, std::string* why);

// One action, its number left to the relay's link: schema's ActionsDiff of that action alone (sync/actions.h), in this
// build's format.
std::string actionFrame(const sync::Action& action);
bool readAction(const std::string& body, sync::Action* out, std::string* why);

std::string historyFrame(sync::SessionId id, std::uint64_t epoch, std::uint64_t before);
bool readHistory(const std::string& body, sync::SessionId* id, std::uint64_t* epoch, std::uint64_t* before,
                 std::string* why);

// The relay's end of the link's state: each frame says the phase, the problem, heardMsAgo and loopback, and the sessions
// as a diff from those of the frame before (Sessions::diff_from in their format), or whole (init_diff) in the first
// frame and whenever the format changed.
class StateWriter {
public:
    std::string frame(const LinkState& state);

private:
    bool sent_ = false;
    sync::Sessions last_;
};

// The window's end: applies each frame to the state it holds. A frame that cannot be applied (malformed, or a diff from
// sessions other than those held) changes nothing and says why; the stream is then not to be trusted.
class StateReader {
public:
    bool apply(const std::string& body, std::string* why);
    const LinkState& state() const { return state_; }

private:
    bool whole_ = false;    // a whole frame has been applied
    LinkState state_;
};

// Overwrites a string's bytes before it is freed: a frame that carried a secret's value (sync/secrets.h).
void wipe(std::string* bytes);

} // namespace nexus::relay
