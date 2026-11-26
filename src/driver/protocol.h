// The driver protocol (docs/driver-protocol.md): the lines nexus-server and an AI's driver process exchange over
// the driver's standard input and output. One JSON object per line, UTF-8, both directions. This file is the messages,
// their encoding and decoding, and a reader that cuts a byte stream into messages. Pure: no OS, no time, no processes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nexus::driver {

// The version a driver states in its hello. A server refuses a driver that states another. Steering (steer,
// follow-up, queue, settled) came into version 1 as kinds an older reader skips, offered only to a driver whose hello
// names them, so the version stays 1 (docs/driver-protocol.md, "Steering a turn").
constexpr int kProtocolVersion = 1;

// The features a driver's hello may name for steering (docs/driver-protocol.md, "Steering a turn"): it takes steer
// lines, it takes follow-up lines, and it writes settled before each done (and queue on every change of its queue). A
// driver that names steer or follow-up names settled too.
constexpr std::string_view kFeatureSteer = "steer";
constexpr std::string_view kFeatureFollowUp = "follow-up";
constexpr std::string_view kFeatureSettled = "settled";

// The longest line either side writes or reads, in bytes, not counting its newline. A longer line is refused, never
// buffered: 1 MiB holds any prompt a person types or pastes (a quarter of a million tokens, past most context windows),
// while a driver streams its replies in deltas far smaller. A reader holds at most this much of one line.
constexpr std::size_t kMaxLineBytes = 1u << 20;

// The largest integer a JSON number (a double) holds exactly: the highest turn number and token count.
constexpr std::uint64_t kMaxInteger = (std::uint64_t(1) << 53) - 1;
constexpr std::uint64_t kMaxTurn = kMaxInteger;

// A kind of message this version does not know, kept by name and otherwise skipped (forward compatibility).
struct Unknown {
    std::string kind;
    bool operator==(const Unknown&) const = default;
};

// ---- Server to driver ----

// A prompt: opens turn `turn`, which the server numbers from 1, strictly increasing within a session and never
// reused.
struct Send {
    std::uint64_t turn = 0;
    std::string prompt;
    bool operator==(const Send&) const = default;
};

// Stop turn `turn`, if it is the open turn; any other stop is ignored.
struct Stop {
    std::uint64_t turn = 0;
    bool operator==(const Stop&) const = default;
};

// Model and effort for the turns after this message (a turn never changes them halfway); an empty field stays as it
// is, and at least one is given. Values are the harness's own names. A driver that does not know this kind skips it.
struct Settings {
    std::string model;
    std::string effort;
    bool operator==(const Settings&) const = default;
};

// What this server takes beyond version 1, written right after the driver's hello: "secrets" means it takes
// ask-secret (docs/secrets.md). A driver that does not know this kind skips it, and never asks.
struct ServerInfo {
    std::vector<std::string> features;
    bool operator==(const ServerInfo&) const = default;
};

// The one answer to an ask-secret: stored, or refused with its reason. Never the value.
struct SecretAnswer {
    std::uint64_t ask = 0;
    bool stored = false;
    std::string reason;         // with refused: busy, no-client, cancelled, timeout or malformed (an older server:
                                // unwritable); with stored, empty (an older server: unconfirmed)
    bool operator==(const SecretAnswer&) const = default;
};

// A message from the person into the open turn `turn`, taken in by the harness after the tool calls running finish and
// before its next model call (or at the end of a model run, before the run would end). `id` is the server's own number
// for it, from 1, strictly increasing within the driver's process. Written only to a driver whose hello named "steer".
struct Steer {
    std::uint64_t turn = 0;
    std::uint64_t id = 0;
    std::string text;
    bool operator==(const Steer&) const = default;
};

// A message from the person into the open turn `turn`, taken in only when the turn would otherwise settle: at the end
// of a model run with no tool calls and no steer pending, one per model run. Written only to a driver whose hello named
// "follow-up".
struct FollowUp {
    std::uint64_t turn = 0;
    std::uint64_t id = 0;
    std::string text;
    bool operator==(const FollowUp&) const = default;
};

using ServerMessage = std::variant<Send, Stop, Settings, ServerInfo, SecretAnswer, Steer, FollowUp, Unknown>;

// ---- Driver to server ----

// The first line a driver writes.
struct Hello {
    int protocol = kProtocolVersion;
    std::string harness;        // the harness's name, as "claude"
    // What the driver supports past the protocol's base: "limit" for `--limit cwd` (docs/driver-protocol.md). The
    // server never runs a limited session on a driver that does not list "limit". Absent: none.
    std::vector<std::string> features = {};
    bool operator==(const Hello&) const = default;
};

// What the driver's account bills: a subscription's login, an API key, or nothing it can use.
enum class Billing { Subscription, ApiKey, None };

// Which credential the harness is using, and its plan as the harness names it (empty when it names none).
struct Credential {
    Billing billing = Billing::None;
    std::string plan;
    bool operator==(const Credential&) const = default;
};

// The harness's session id: what the server passes as --resume when it starts the driver again.
struct Session {
    std::string id;
    bool operator==(const Session&) const = default;
};

// A piece of the turn's current text block, to append to it.
struct Text {
    std::uint64_t turn = 0;
    std::string delta;
    bool operator==(const Text&) const = default;
};

// The turn's current text block is complete.
struct TextEnd {
    std::uint64_t turn = 0;
    bool operator==(const TextEnd&) const = default;
};

// The AI used a tool: its name and one line saying what it did.
struct ChildAgents {
    std::uint32_t count;
    bool operator==(const ChildAgents&) const = default;
};

struct Tool {
    std::uint64_t turn = 0;
    std::string name;
    std::string summary;
    bool operator==(const Tool&) const = default;
};

enum class Outcome { Ok, Stopped, Error };

// Token counts the harness reported with the turn's result; a count it did not report is absent.
struct Usage {
    std::optional<std::uint64_t> input;
    std::optional<std::uint64_t> output;
    std::optional<std::uint64_t> cacheRead;
    std::optional<std::uint64_t> cacheWrite;
    bool operator==(const Usage&) const = default;
};

// The turn has ended; the last event of its turn. `sessionCostUsd` is the harness's running estimate for the whole
// session so far, not for this turn.
struct Done {
    std::uint64_t turn = 0;
    Outcome outcome = Outcome::Ok;
    std::optional<double> sessionCostUsd;
    std::optional<Usage> usage;
    bool operator==(const Done&) const = default;
};

// What kind of error: a broken protocol rule, no usable credential, a --resume id the harness does not know, the
// harness's own failure, or anything else (also what a code this version does not know reads as).
enum class ErrorCode { Protocol, NoCredential, SessionNotFound, Harness, UsageLimit, ModelLimit, Other };

// Something went wrong: what, and what a person does about it. `turn` when it belongs to one.
struct Error {
    std::string cause;
    std::string fix;
    std::optional<std::uint64_t> turn;
    ErrorCode code = ErrorCode::Other;
    bool operator==(const Error&) const = default;
};

// An ask for a secret for an account (docs/secrets.md), from the AI's tool call: `ask` is the driver's own number for
// it, unique within its process; `what` a file the account's provider documents; `why` one line the dialog shows.
struct AskSecret {
    std::uint64_t ask = 0;
    std::string account;
    std::string what;
    std::string why;
    bool operator==(const AskSecret&) const = default;
};

// The driver's whole queue for turn `turn`: the ids of the steers and of the follow-ups it took and has not yet given
// the harness, oldest first. Written after every change of either list, never a diff: an id leaves only when the
// harness takes it in.
struct Queue {
    std::uint64_t turn = 0;
    std::vector<std::uint64_t> steer;
    std::vector<std::uint64_t> followUp;
    bool operator==(const Queue&) const = default;
};

// Turn `turn` has no automatic work left (no retry, no compaction, nothing queued taken in): written once, just before
// its done, by a driver whose hello named "settled". `aborted` when a stop ended it: then what its last queue held was
// never given the harness.
struct Settled {
    std::uint64_t turn = 0;
    bool aborted = false;
    bool operator==(const Settled&) const = default;
};

using DriverMessage = std::variant<Hello, Credential, Session, Text, TextEnd, Tool, ChildAgents, Done, Error, AskSecret,
    Queue, Settled, Unknown>;

// ---- Encoding and decoding ----

// A message as one line ending in a newline, or why it cannot be written: whatever the decoder would refuse (a line
// longer than kMaxLineBytes, a field out of range, a string that is not UTF-8), anything that would not read back
// equal, and an Unknown, which is only ever read.
struct Encoded {
    std::string line;
    std::string error;          // empty when `line` holds the message
};

Encoded encode(const ServerMessage& message);
Encoded encode(const DriverMessage& message);

// One line, without its newline, as a message, or the cause of its refusal: not UTF-8 (an escaped lone surrogate
// included), a byte-order mark, not JSON, nested too deeply, not an object, no "kind", or a known kind with a required
// field missing or of the wrong type or value. Unknown fields are ignored, of duplicate keys the last wins, and an
// unknown kind decodes as Unknown.
template <class Message>
struct Decoded {
    std::optional<Message> message;
    std::string error;          // empty when `message` holds one
};

Decoded<ServerMessage> decodeServerMessage(std::string_view line);
Decoded<DriverMessage> decodeDriverMessage(std::string_view line);

// ---- Reading a stream ----

// Cuts the bytes of a stream, in chunks of any size, into lines. A line longer than kMaxLineBytes is refused when its
// limit is passed: the reader reports it once, drops the rest of it up to its newline, and goes on with the next line.
// Each line is one item: its text, or the cause it was refused for. A line may end in CR LF; the CR is dropped (it
// counts toward the limit). Decoding is the caller's (decodeServerMessage or decodeDriverMessage), so one reader serves
// both ends.
class LineReader {
public:
    struct Line {
        std::string text;       // the line without its newline (or CR LF), when `error` is empty
        std::string error;
        std::uint64_t number = 0;   // its place in the stream, from 1
    };

    // Appends what `bytes` completes to `out`.
    void feed(std::string_view bytes, std::vector<Line>& out);

    // The stream ended. A line left without its newline is refused: a writer ends every line.
    void finish(std::vector<Line>& out);

private:
    std::string partial_;       // the current line so far, at most kMaxLineBytes
    bool dropping_ = false;     // the current line passed the limit; drop up to its newline
    std::uint64_t lineNumber_ = 1;
};

// A LineReader that decodes as it reads: the messages, or the cause each bad line was refused for (which names the
// line's number), in stream order.
template <class Message>
class MessageReader {
public:
    using Item = Decoded<Message>;

    void feed(std::string_view bytes, std::vector<Item>& out) {
        lines_.clear();
        reader_.feed(bytes, lines_);
        decode(out);
    }

    void finish(std::vector<Item>& out) {
        lines_.clear();
        reader_.finish(lines_);
        decode(out);
    }

private:
    void decode(std::vector<Item>& out);

    LineReader reader_;
    std::vector<LineReader::Line> lines_;
};

using ServerReader = MessageReader<ServerMessage>;   // what a driver reads
using DriverReader = MessageReader<DriverMessage>;   // what the server reads

// The most ids one list of a queue line holds: a driver queues no more than this per turn, so a queue line stays far
// within the line limit. The server writes no more than this many steers and follow-ups into one turn.
constexpr std::size_t kMaxQueued = 64;

// The names the protocol gives these values, as written on the line.
std::string_view billingName(Billing billing);
std::string_view outcomeName(Outcome outcome);
std::string_view errorCodeName(ErrorCode code);

} // namespace nexus::driver
