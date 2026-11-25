// Unit tests of the driver protocol (src/driver/protocol.h): every message's round trip, the encoder refusing what the
// decoder refuses (and a property test over random messages), the reader over every chunking, each refusal with its
// cause, forward compatibility, and the line limit at its edges. Logic only: no time, no sockets, no processes, no
// files.
//
//   driver_protocol_test           runs every group
//   driver_protocol_test <group>   runs one

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "driver/protocol.h"

namespace {

using namespace nexus::driver;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

// splitmix64: the same chunkings and messages on every run.
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

bool contains(const std::string& s, std::string_view part) {
    return s.find(part) != std::string::npos;
}

// Strings that test the escaping: quotes, backslashes, line breaks, control characters, and text past ASCII.
const std::vector<std::string> kTexts = {"", "hi", "two\nlines\r\n", "\"q\" \\ /", "tab\there \x01\x1f",
    "\xc3\xa9t\xc3\xa9 \xe2\x9c\x93 \xf0\x9f\x9a\x80", "\xef\xbf\xbd"};

std::vector<ServerMessage> serverMessages() {
    std::vector<ServerMessage> all = {Stop{3}, Stop{kMaxTurn}, Send{kMaxTurn, "last"},
        Settings{"claude-opus-5-5", "high"}, Settings{"opus", ""}, Settings{"", "low"},
        ServerInfo{{"secrets"}}, ServerInfo{{}}, ServerInfo{{"secrets", "a feature from the future"}},
        SecretAnswer{1, true, ""}, SecretAnswer{kMaxInteger, false, "busy"}, SecretAnswer{3, false, "no-client"},
        SecretAnswer{5, true, "unconfirmed"}};
    std::uint64_t turn = 1;
    for (const std::string& t : kTexts)
        all.push_back(Send{turn++, t});
    return all;
}

std::vector<DriverMessage> driverMessages() {
    std::vector<DriverMessage> all = {
        Hello{1, "claude"},
        Hello{7, "a harness from the future"},
        Hello{1, "claude", {"limit"}},
        Credential{Billing::Subscription, "max"},
        Credential{Billing::ApiKey, ""},
        Credential{Billing::None, ""},
        Session{"5f0c2a8e-1d34-4b7e-9a51-3c6f2b0d9e77"},
        Session{std::string(256, '~')},
        ChildAgents{3}, ChildAgents{0}, TextEnd{3},
        Tool{3, "Bash", "ls -la src"},
        Tool{3, "Read", ""},
        Tool{3, "Edit", "caf\xc3\xa9 \xe2\x9c\x93"},
        Done{3, Outcome::Ok, std::nullopt, std::nullopt},
        Done{4, Outcome::Stopped, 0.0, Usage{}},
        Done{5, Outcome::Error, 1.234567, Usage{10, 20, 30, 40}},
        Done{6, Outcome::Ok, 0.1, Usage{0, std::nullopt, kMaxInteger, std::nullopt}},
        Error{"the cause", "the fix", std::nullopt, ErrorCode::Other},
        Error{"a turn's cause", "its fix", 9, ErrorCode::Protocol},
        Error{"no login", "log in", std::nullopt, ErrorCode::NoCredential},
        Error{"Fable limit reached", "choose account or Opus", 3, ErrorCode::ModelLimit},
        Error{"no such session", "start without --resume", std::nullopt, ErrorCode::SessionNotFound},
        Error{"the harness failed", "see its log", 9, ErrorCode::Harness},
        AskSecret{1, "work", "oauth-token", "the login expired"},
        AskSecret{kMaxInteger, "caf\xc3\xa9", "oauth-token", "\"quoted\" \\ \xe2\x9c\x93"},
        AskSecret{2, "a", "b", ""},
    };
    for (const std::string& t : kTexts)
        all.push_back(Text{2, t});
    return all;
}

// A message as text: its line, or the name of an unknown kind (which cannot be encoded).
template <class Message>
std::string shown(const Message& m) {
    if (const Unknown* u = std::get_if<Unknown>(&m))
        return "U " + u->kind + "\n";
    return "M " + encode(m).line;
}

// What a reader yields, as text: each message, or its error.
template <class Item>
std::string show(const std::vector<Item>& items) {
    std::string s;
    for (const Item& item : items)
        s += item.message ? shown(*item.message) : "E " + item.error + "\n";
    return s;
}

std::string driverError(std::string_view line) {
    const Decoded<DriverMessage> d = decodeDriverMessage(line);
    CHECK(!d.message);
    return d.error;
}

std::string serverError(std::string_view line) {
    const Decoded<ServerMessage> d = decodeServerMessage(line);
    CHECK(!d.message);
    return d.error;
}

std::string encodeError(const DriverMessage& m) {
    const Encoded e = encode(m);
    CHECK(e.line.empty());
    return e.error;
}

// ---- round trips ----

void roundtrip() {
    for (const ServerMessage& m : serverMessages()) {
        const Encoded e = encode(m);
        CHECK(e.error.empty());
        CHECK(!e.line.empty() && e.line.back() == '\n');
        CHECK(e.line.find('\n') == e.line.size() - 1);
        const Decoded<ServerMessage> d = decodeServerMessage(std::string_view(e.line).substr(0, e.line.size() - 1));
        CHECK(d.error.empty());
        CHECK(d.message && *d.message == m);
    }
    for (const DriverMessage& m : driverMessages()) {
        const Encoded e = encode(m);
        CHECK(e.error.empty());
        CHECK(e.line.find('\n') == e.line.size() - 1);
        const Decoded<DriverMessage> d = decodeDriverMessage(std::string_view(e.line).substr(0, e.line.size() - 1));
        CHECK(d.error.empty());
        CHECK(d.message && *d.message == m);
    }
    // The wire names, as the document gives them.
    CHECK(encode(ServerMessage{Send{1, "hi"}}).line == "{\"kind\":\"send\",\"turn\":1,\"prompt\":\"hi\"}\n");
    CHECK(encode(ServerMessage{Stop{4}}).line == "{\"kind\":\"stop\",\"turn\":4}\n");
    CHECK(encode(ServerMessage{Settings{"opus", "max"}}).line ==
        "{\"kind\":\"settings\",\"model\":\"opus\",\"effort\":\"max\"}\n");
    CHECK(encode(ServerMessage{Settings{"", "low"}}).line == "{\"kind\":\"settings\",\"effort\":\"low\"}\n");
    const Decoded<ServerMessage> settingsNull =
        decodeServerMessage("{\"kind\":\"settings\",\"model\":null,\"effort\":\"low\"}");
    CHECK(settingsNull.message && *settingsNull.message == ServerMessage(Settings{"", "low"}));
    CHECK(encode(DriverMessage{TextEnd{2}}).line == "{\"kind\":\"text-end\",\"turn\":2}\n");
    CHECK(encode(DriverMessage{Credential{Billing::ApiKey, ""}}).line ==
        "{\"kind\":\"credential\",\"billing\":\"api-key\"}\n");
    CHECK(encode(DriverMessage{Done{1, Outcome::Stopped, 0.5, Usage{1, 2, 3, 4}}}).line ==
        "{\"kind\":\"done\",\"turn\":1,\"outcome\":\"stopped\",\"session_cost_usd\":0.5,\"usage\":{\"input_tokens\":1,"
        "\"output_tokens\":2,\"cache_read_tokens\":3,\"cache_write_tokens\":4}}\n");
    CHECK(encode(DriverMessage{Error{"c", "f", 2, ErrorCode::SessionNotFound}}).line ==
        "{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"turn\":2,\"code\":\"session-not-found\"}\n");
    // An optional field written as null reads as absent; an error with no code, or one this version does not know,
    // reads as other.
    const Decoded<DriverMessage> d =
        decodeDriverMessage("{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"turn\":null,\"plan\":null}");
    CHECK(d.message && *d.message == DriverMessage(Error{"c", "f", std::nullopt, ErrorCode::Other}));
    const Decoded<DriverMessage> later =
        decodeDriverMessage("{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"code\":\"rate-limited\"}");
    CHECK(later.message && *later.message == DriverMessage(Error{"c", "f", std::nullopt, ErrorCode::Other}));
    const Decoded<DriverMessage> no =
        decodeDriverMessage("{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"code\":\"no-credential\"}");
    CHECK(no.message && *no.message == DriverMessage(Error{"c", "f", std::nullopt, ErrorCode::NoCredential}));
    // The account's usage limit: the server moves the session to its friend's next signed-in account.
    const Decoded<DriverMessage> limit =
        decodeDriverMessage("{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"turn\":3,\"code\":\"usage-limit\"}");
    CHECK(limit.message && *limit.message == DriverMessage(Error{"c", "f", 3, ErrorCode::UsageLimit}));
    CHECK(encode(DriverMessage{Error{"c", "f", 3, ErrorCode::UsageLimit}}).line ==
        "{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"turn\":3,\"code\":\"usage-limit\"}\n");
}

// The encoder refuses everything the decoder would refuse, with its cause, and writes nothing.
void encode_refusals() {
    CHECK(contains(encode(ServerMessage{Send{0, "x"}}).error, "\"turn\""));
    CHECK(contains(encode(ServerMessage{Stop{0}}).error, "\"turn\""));
    CHECK(contains(encode(ServerMessage{Unknown{"pause"}}).error, "unknown kind"));
    CHECK(contains(encode(ServerMessage{Settings{"", ""}}).error, "neither"));
    CHECK(contains(encodeError(Unknown{"thinking"}), "unknown kind"));
    CHECK(contains(encodeError(Text{kMaxTurn + 1, "x"}), "\"turn\""));
    CHECK(contains(encodeError(Hello{0, "x"}), "\"protocol\""));
    CHECK(contains(encodeError(Hello{1, ""}), "empty \"harness\""));
    CHECK(contains(encodeError(Session{""}), "\"id\""));
    CHECK(contains(encodeError(Session{"a b"}), "\"id\""));
    CHECK(contains(encodeError(Session{"-x"}), "\"id\""));
    CHECK(contains(encodeError(Tool{1, "", "s"}), "empty \"name\""));
    CHECK(contains(encodeError(Tool{1, "Bash", "two\nlines"}), "\"summary\""));
    CHECK(contains(encodeError(Tool{1, "Bash", "a\rb"}), "\"summary\""));
    CHECK(contains(encodeError(Error{"", "f", std::nullopt, ErrorCode::Other}), "empty \"cause\""));
    CHECK(contains(encodeError(Error{"c", "", std::nullopt, ErrorCode::Other}), "empty \"fix\""));
    CHECK(contains(encodeError(Error{"c", "f", 0, ErrorCode::Other}), "\"turn\""));
    CHECK(contains(encodeError(Done{1, Outcome::Ok, -1.0, std::nullopt}), "\"session_cost_usd\""));
    CHECK(!encodeError(Done{1, Outcome::Ok, std::nan(""), std::nullopt}).empty());
    CHECK(!encodeError(Done{1, Outcome::Ok, std::numeric_limits<double>::infinity(), std::nullopt}).empty());
    CHECK(contains(encodeError(Done{1, Outcome::Ok, std::nullopt, Usage{kMaxInteger + 1, {}, {}, {}}}),
        "input_tokens"));
    CHECK(!encodeError(Text{1, "bad\xff"}).empty());
    CHECK(!encodeError(Credential{Billing(7), ""}).empty());
    CHECK(!encodeError(Done{1, Outcome(9), std::nullopt, std::nullopt}).empty());
}

// A random string: mostly text, sometimes awkward bytes (control characters, separators, ill-formed UTF-8).
std::string randomString(Rng& rng) {
    static const std::vector<std::string> pieces = {"a", "Bash", " ", "-", "\n", "\r", "\t", "\x7f", "\"", "\\",
        "\xc3\xa9", "\xe2\x80\xa8", "\xc2\x85", "\xf0\x9f\x9a\x80", "\xff", "\xed\xa0\x80", "\xe2\x9c", "\xef\xbb\xbf"};
    std::string s;
    const std::size_t n = rng.below(6);
    for (std::size_t i = 0; i < n; ++i)
        s += pieces[rng.below(pieces.size())];
    return s;
}

std::optional<std::uint64_t> randomCount(Rng& rng) {
    switch (rng.below(4)) {
        case 0: return std::nullopt;
        case 1: return rng.below(100000);
        case 2: return kMaxInteger + rng.below(2);
        default: return 0;
    }
}

std::uint64_t randomTurn(Rng& rng) {
    const std::uint64_t edges[] = {0, 1, 2, kMaxTurn, kMaxTurn + 1};
    return rng.below(2) == 0 ? edges[rng.below(5)] : 1 + rng.below(1000);
}

DriverMessage randomDriverMessage(Rng& rng) {
    switch (rng.below(9)) {
        case 0: return Hello{int(rng.below(3)), randomString(rng)};
        case 1: return Credential{Billing(rng.below(4)), randomString(rng)};
        case 2: return Session{randomString(rng)};
        case 3: return Text{randomTurn(rng), randomString(rng)};
        case 4: return TextEnd{randomTurn(rng)};
        case 5: return Tool{randomTurn(rng), randomString(rng), randomString(rng)};
        case 6: {
            const double costs[] = {0.0, 0.25, -1.0, 1e300, std::nan(""), std::numeric_limits<double>::infinity()};
            Done d{randomTurn(rng), Outcome(rng.below(4)), std::nullopt, std::nullopt};
            if (rng.below(2) == 0)
                d.sessionCostUsd = costs[rng.below(6)];
            if (rng.below(2) == 0)
                d.usage = Usage{randomCount(rng), randomCount(rng), randomCount(rng), randomCount(rng)};
            return d;
        }
        case 7: {
            Error e{randomString(rng), randomString(rng), std::nullopt, ErrorCode(rng.below(6))};
            if (rng.below(2) == 0)
                e.turn = randomTurn(rng);
            return e;
        }
        default: return Unknown{randomString(rng)};
    }
}

// Every message the encoder accepts, the decoder accepts and gives back equal.
void property() {
    Rng rng{7};
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        const DriverMessage m = randomDriverMessage(rng);
        const Encoded e = encode(m);
        CHECK(e.line.empty() != e.error.empty());
        if (e.line.empty())
            continue;
        ++accepted;
        const Decoded<DriverMessage> d = decodeDriverMessage(std::string_view(e.line).substr(0, e.line.size() - 1));
        CHECK(d.message && *d.message == m);
        if (!d.message || !(*d.message == m))
            std::fprintf(stderr, "  encoded but not read back equal: %s", e.line.c_str());
    }
    for (int i = 0; i < 5000; ++i) {
        const ServerMessage m = rng.below(2) == 0 ? ServerMessage{Send{randomTurn(rng), randomString(rng)}}
                                                  : ServerMessage{Stop{randomTurn(rng)}};
        const Encoded e = encode(m);
        if (e.line.empty())
            continue;
        ++accepted;
        const Decoded<ServerMessage> d = decodeServerMessage(std::string_view(e.line).substr(0, e.line.size() - 1));
        CHECK(d.message && *d.message == m);
    }
    CHECK(accepted > 5000);     // the generator reaches both sides
}

// ---- the reader ----

// A stream of every message, bad lines between them, and a last line without its newline.
std::string stream() {
    std::string s;
    for (const DriverMessage& m : driverMessages())
        s += encode(m).line;
    s += "{\"kind\":\"thinking\",\"turn\":2}\r\n";
    s += "not json\n";
    s += "\n";
    s += "{\"kind\":\"text\",\"turn\":\"one\",\"delta\":\"x\"}\n";
    s += "{\"kind\":\"text\",\"turn\":1,\"delta\":\"\xc3\"}\n";
    s += encode(DriverMessage{Text{1, std::string(5000, 'y')}}).line;
    s += "{\"kind\":\"text-end\",\"turn\":1}";
    return s;
}

void chunking() {
    const std::string s = stream();
    DriverReader whole;
    std::vector<DriverReader::Item> expect;
    whole.feed(s, expect);
    whole.finish(expect);
    CHECK(expect.size() == driverMessages().size() + 7);
    CHECK(expect.back().message == std::nullopt && contains(expect.back().error, "ended inside line"));

    DriverReader bytewise;
    std::vector<DriverReader::Item> got;
    for (char c : s)
        bytewise.feed(std::string_view(&c, 1), got);
    bytewise.finish(got);
    CHECK(show(got) == show(expect));

    for (std::uint64_t seed = 1; seed <= 50; ++seed) {
        Rng rng{seed};
        DriverReader reader;
        std::vector<DriverReader::Item> items;
        std::size_t at = 0;
        while (at < s.size()) {
            // mostly small chunks, now and then a large one that holds many lines
            const std::size_t n = rng.below(8) == 0 ? rng.below(20000) : rng.below(40);
            reader.feed(std::string_view(s).substr(at, n), items);
            at += std::min(n, s.size() - at);
        }
        reader.finish(items);
        CHECK(show(items) == show(expect));
    }

    // The server's direction reads the same way.
    std::string server;
    for (const ServerMessage& m : serverMessages())
        server += encode(m).line;
    ServerReader a;
    ServerReader b;
    std::vector<ServerReader::Item> all;
    std::vector<ServerReader::Item> one;
    a.feed(server, all);
    for (char c : server)
        b.feed(std::string_view(&c, 1), one);
    CHECK(all.size() == serverMessages().size());
    CHECK(show(one) == show(all));

    // A tail of one byte with no newline is refused at the end of the stream.
    LineReader tail;
    std::vector<LineReader::Line> out;
    tail.feed("x", out);
    CHECK(out.empty());
    tail.finish(out);
    CHECK(out.size() == 1 && contains(out[0].error, "ended inside line 1 (1 bytes"));
}

// ---- refusals ----

void refusals() {
    // not UTF-8: a stray continuation byte, an overlong form, an encoded surrogate, a truncated sequence
    for (std::string_view bad : {"{\"kind\":\"x\x80\"}", "{\"kind\":\"\xc0\xaf\"}", "{\"kind\":\"\xed\xa0\x80\"}",
             "{\"kind\":\"\xe2\x9c\"}"})
        CHECK(contains(driverError(bad), "not UTF-8"));
    CHECK(contains(driverError("{\"kind\":\"x\x80\"}"), "at byte 10"));
    // an escaped lone surrogate, like the raw one; a pair is one character
    for (std::string_view bad : {"\\ud83d", "\\ude80", "\\ud83dx", "\\ud83d\\u0041", "\\uDBFF\\uDBFF", "\\udc00",
             "\\udfff", "\\ud800\\n", "\\ud800\\\\udc00"})
        CHECK(contains(driverError("{\"kind\":\"text\",\"turn\":1,\"delta\":\"" + std::string(bad) + "\"}"),
            "lone surrogate"));
    const Decoded<DriverMessage> pair =
        decodeDriverMessage("{\"kind\":\"text\",\"turn\":1,\"delta\":\"\\ud83d\\ude80\"}");
    CHECK(pair.message && *pair.message == DriverMessage(Text{1, "\xf0\x9f\x9a\x80"}));
    const Decoded<DriverMessage> escapedBackslash =
        decodeDriverMessage("{\"kind\":\"text\",\"turn\":1,\"delta\":\"\\\\ud83d\"}");
    CHECK(escapedBackslash.message && *escapedBackslash.message == DriverMessage(Text{1, "\\ud83d"}));
    // a settings with nothing to change, an empty value, or a value that is not a string
    CHECK(contains(serverError("{\"kind\":\"settings\"}"), "neither \"model\" nor \"effort\""));
    CHECK(contains(serverError("{\"kind\":\"settings\",\"model\":\"\"}"), "empty \"model\""));
    CHECK(contains(serverError("{\"kind\":\"settings\",\"effort\":3}"), "\"effort\" that is not a string"));
    // a byte-order mark
    CHECK(contains(serverError("\xEF\xBB\xBF{\"kind\":\"stop\",\"turn\":1}"), "byte-order mark"));
    // not JSON, nested too deeply, or not one object
    CHECK(contains(driverError(""), "not JSON"));
    CHECK(contains(driverError("{\"kind\":\"stop\""), "not JSON"));
    CHECK(contains(driverError("{\"kind\":\"stop\"} {}"), "not JSON"));
    CHECK(contains(serverError(std::string_view("{\"kind\":\"st\0op\"}", 16)), "not JSON"));
    // The object and 1023 arrays inside it are the bound, 1024 arrays one past it.
    auto nested = [](std::size_t arrays) {
        return "{\"kind\":\"stop\",\"turn\":1,\"x\":" + std::string(arrays, '[') + std::string(arrays, ']') + "}";
    };
    const Decoded<ServerMessage> atBound = decodeServerMessage(nested(1023));
    CHECK(atBound.message && *atBound.message == ServerMessage(Stop{1}));
    CHECK(contains(serverError(nested(1024)), "nested too deeply"));
    CHECK(contains(serverError(nested(1100)), "nested too deeply"));
    CHECK(contains(serverError("{\"kind\":\"stop\",\"turn\":1,\"x\":" + std::string(500000, '[')),
        "nested too deeply"));
    CHECK(contains(serverError("[1,2]"), "not an object"));
    CHECK(contains(serverError("\"send\""), "not an object"));
    CHECK(contains(serverError("{\"turn\":1}"), "no \"kind\""));
    CHECK(contains(serverError("{\"kind\":3}"), "no \"kind\""));
    CHECK(contains(serverError("{\"kind\":\"\"}"), "no \"kind\""));

    // missing required fields, each kind
    CHECK(contains(serverError("{\"kind\":\"send\",\"prompt\":\"x\"}"), "a \"send\" message has no \"turn\""));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":1}"), "has no \"prompt\""));
    CHECK(contains(serverError("{\"kind\":\"stop\"}"), "a \"stop\" message has no \"turn\""));
    CHECK(contains(driverError("{\"kind\":\"hello\",\"harness\":\"x\"}"), "has no \"protocol\""));
    CHECK(contains(driverError("{\"kind\":\"hello\",\"protocol\":1}"), "has no \"harness\""));
    CHECK(contains(driverError("{\"kind\":\"credential\"}"), "has no \"billing\""));
    CHECK(contains(driverError("{\"kind\":\"session\"}"), "has no \"id\""));
    CHECK(contains(driverError("{\"kind\":\"text\",\"turn\":1}"), "has no \"delta\""));
    CHECK(contains(driverError("{\"kind\":\"text\",\"delta\":\"x\"}"), "has no \"turn\""));
    CHECK(contains(driverError("{\"kind\":\"text-end\"}"), "has no \"turn\""));
    CHECK(contains(driverError("{\"kind\":\"tool\",\"turn\":1,\"summary\":\"\"}"), "has no \"name\""));
    CHECK(contains(driverError("{\"kind\":\"tool\",\"turn\":1,\"name\":\"x\"}"), "has no \"summary\""));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1}"), "has no \"outcome\""));
    CHECK(contains(driverError("{\"kind\":\"done\",\"outcome\":\"ok\"}"), "has no \"turn\""));
    CHECK(contains(driverError("{\"kind\":\"error\",\"fix\":\"f\"}"), "has no \"cause\""));
    CHECK(contains(driverError("{\"kind\":\"error\",\"cause\":\"c\"}"), "has no \"fix\""));

    // wrong types and values
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":\"1\",\"prompt\":\"x\"}"),
        "\"turn\" that is not a whole number"));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":0,\"prompt\":\"x\"}"), "from 1 to"));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":1.5,\"prompt\":\"x\"}"), "not a whole number"));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":-1,\"prompt\":\"x\"}"), "not a whole number"));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":9007199254740992,\"prompt\":\"x\"}"), "not a whole"));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":1e400,\"prompt\":\"x\"}"), "not a whole number"));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":1,\"prompt\":7}"), "\"prompt\" that is not a string"));
    CHECK(contains(serverError("{\"kind\":\"stop\",\"turn\":0}"), "\"turn\""));
    CHECK(contains(driverError("{\"kind\":\"hello\",\"protocol\":0,\"harness\":\"x\"}"), "\"protocol\""));
    CHECK(contains(driverError("{\"kind\":\"hello\",\"protocol\":1,\"harness\":\"\"}"), "empty \"harness\""));
    CHECK(contains(driverError("{\"kind\":\"credential\",\"billing\":\"card\"}"), "subscription, api-key, none"));
    CHECK(contains(driverError("{\"kind\":\"credential\",\"billing\":\"none\",\"plan\":1}"), "\"plan\""));
    CHECK(contains(driverError("{\"kind\":\"session\",\"id\":\"\"}"), "\"id\""));
    CHECK(contains(driverError("{\"kind\":\"text\",\"turn\":1,\"delta\":null}"), "has no \"delta\""));
    CHECK(contains(driverError("{\"kind\":\"tool\",\"turn\":1,\"name\":\"\",\"summary\":\"\"}"), "empty \"name\""));
    CHECK(contains(driverError("{\"kind\":\"tool\",\"turn\":1,\"name\":\"x\",\"summary\":\"a\\nb\"}"), "\"summary\""));
    CHECK(contains(driverError("{\"kind\":\"tool\",\"turn\":1,\"name\":\"x\",\"summary\":\"a\\rb\"}"), "\"summary\""));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1,\"outcome\":\"cancelled\"}"), "ok, stopped, error"));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1,\"outcome\":\"ok\",\"session_cost_usd\":-0.5}"),
        "\"session_cost_usd\""));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1,\"outcome\":\"ok\",\"session_cost_usd\":\"1\"}"),
        "\"session_cost_usd\""));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1,\"outcome\":\"ok\",\"session_cost_usd\":1e999}"),
        "\"session_cost_usd\" that is not a finite number"));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1,\"outcome\":\"ok\",\"usage\":[]}"),
        "\"usage\" that is not an object"));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1,\"outcome\":\"ok\",\"usage\":{\"input_tokens\":-1}}"),
        "\"input_tokens\""));
    CHECK(contains(driverError("{\"kind\":\"done\",\"turn\":1,\"outcome\":\"ok\",\"usage\":{\"output_tokens\":"
        "9007199254740992}}"), "\"output_tokens\""));
    const Decoded<DriverMessage> most = decodeDriverMessage(
        "{\"kind\":\"done\",\"turn\":1,\"outcome\":\"ok\",\"usage\":{\"output_tokens\":9007199254740991}}");
    CHECK(most.message && std::get<Done>(*most.message).usage->output == kMaxInteger);
    CHECK(contains(driverError("{\"kind\":\"error\",\"cause\":\"\",\"fix\":\"f\"}"), "empty \"cause\""));
    CHECK(contains(driverError("{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"\"}"), "empty \"fix\""));
    CHECK(contains(driverError("{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"turn\":0}"), "\"turn\""));
    CHECK(contains(driverError("{\"kind\":\"error\",\"cause\":\"c\",\"fix\":\"f\",\"code\":3}"), "\"code\""));

    // the reader names the line a refusal came from
    DriverReader reader;
    std::vector<DriverReader::Item> items;
    reader.feed("{\"kind\":\"text-end\",\"turn\":1}\n{\"kind\":\"text-end\"}\n", items);
    CHECK(items.size() == 2 && items[0].message && contains(items[1].error, "line 2: a \"text-end\" message has no"));
    // and a line the stream ended inside
    reader.feed("{\"kind\":", items);
    reader.finish(items);
    CHECK(items.size() == 3 && contains(items[2].error, "stream ended inside line 3 (8 bytes"));
}

// The strings with rules of their own: a session id becomes a command-line argument; a tool's name and summary are
// one line each.
void strings() {
    for (std::string_view id : {"a b", "a\\nb --evil", " ", "tab\\there", "caf\xc3\xa9", "del\x7f", "-", "--resume"})
        CHECK(contains(driverError("{\"kind\":\"session\",\"id\":\"" + std::string(id) + "\"}"), "\"id\""));
    CHECK(contains(driverError("{\"kind\":\"session\",\"id\":\"" + std::string(257, 'a') + "\"}"), "\"id\""));
    const Decoded<DriverMessage> longest =
        decodeDriverMessage("{\"kind\":\"session\",\"id\":\"" + std::string(256, 'a') + "\"}");
    CHECK(longest.message && *longest.message == DriverMessage(Session{std::string(256, 'a')}));
    for (std::string_view bad : {"\\t", "\\r", "\\n", "\\u0000", "\x7f", "\\u0085", "\\u009f", "\\u2028", "\\u2029"}) {
        CHECK(contains(driverError("{\"kind\":\"tool\",\"turn\":1,\"name\":\"a" + std::string(bad) +
            "\",\"summary\":\"\"}"), "\"name\""));
        CHECK(contains(driverError("{\"kind\":\"tool\",\"turn\":1,\"name\":\"a\",\"summary\":\"b" + std::string(bad) +
            "\"}"), "\"summary\""));
    }
    // Text past ASCII is fine in both, and anything but a lone surrogate is fine in a delta.
    const Decoded<DriverMessage> tool = decodeDriverMessage(
        "{\"kind\":\"tool\",\"turn\":1,\"name\":\"\xc3\xa9\",\"summary\":\"\\u2027 \\u00a0 \xe2\x9c\x93\"}");
    CHECK(tool.message);
    const Decoded<DriverMessage> text = decodeDriverMessage(
        "{\"kind\":\"text\",\"turn\":1,\"delta\":\"\\u2028\\u0085\\t\\r\\n\\u0000\"}");
    CHECK(text.message);
}

// Integer fields take a JSON number whose value is a whole number in range, however it is written.
void numbers() {
    for (std::string_view one : {"1", "1.0", "1e0", "10e-1", "0.1e1"}) {
        const Decoded<ServerMessage> d =
            decodeServerMessage("{\"kind\":\"send\",\"turn\":" + std::string(one) + ",\"prompt\":\"x\"}");
        CHECK(d.message && *d.message == ServerMessage(Send{1, "x"}));
    }
    const Decoded<ServerMessage> top =
        decodeServerMessage("{\"kind\":\"send\",\"turn\":9007199254740991,\"prompt\":\"x\"}");
    CHECK(top.message && std::get<Send>(*top.message).turn == kMaxTurn);
    for (std::string_view bad : {"1.5", "-0", "0", "true", "null", "[1]", "9007199254740993"})
        CHECK(!decodeServerMessage("{\"kind\":\"send\",\"turn\":" + std::string(bad) + ",\"prompt\":\"x\"}").message);
}

// Unknown fields are ignored; an unknown kind is kept by name, to be skipped. Of duplicate keys the last wins. A line
// may end in CR LF.
void forward() {
    const Decoded<DriverMessage> d = decodeDriverMessage(
        "{\"future\":{\"x\":[1,{\"y\":null}]},\"kind\":\"text\",\"turn\":4,\"delta\":\"a\",\"lang\":\"en\"}");
    CHECK(d.message && *d.message == DriverMessage(Text{4, "a"}));
    const Decoded<DriverMessage> u = decodeDriverMessage("{\"kind\":\"thinking\",\"turn\":4,\"text\":7}");
    CHECK(u.message && *u.message == DriverMessage(Unknown{"thinking"}));
    const Decoded<ServerMessage> s = decodeServerMessage("{\"kind\":\"pause\",\"for\":\"ever\"}");
    CHECK(s.message && *s.message == ServerMessage(Unknown{"pause"}));
    const Decoded<ServerMessage> stop = decodeServerMessage("{\"kind\":\"stop\",\"turn\":2,\"why\":\"later\"}");
    CHECK(stop.message && *stop.message == ServerMessage(Stop{2}));
    // An unknown kind is still a JSON object with a kind: a malformed one is refused.
    CHECK(contains(driverError("{\"kind\":\"thinking\""), "not JSON"));

    const Decoded<ServerMessage> dup = decodeServerMessage(
        "{\"kind\":\"stop\",\"kind\":\"send\",\"turn\":1,\"prompt\":\"a\",\"turn\":2,\"prompt\":\"b\"}");
    CHECK(dup.message && *dup.message == ServerMessage(Send{2, "b"}));
    CHECK(contains(serverError("{\"kind\":\"send\",\"turn\":1,\"prompt\":\"a\",\"turn\":null}"), "no \"turn\""));

    ServerReader reader;
    std::vector<ServerReader::Item> items;
    reader.feed("{\"kind\":\"stop\",\"turn\":1}\r\n{\"kind\":\"stop\",\"turn\":2}\n\r\n", items);
    CHECK(items.size() == 3);
    if (items.size() == 3) {
        CHECK(items[0].message && *items[0].message == ServerMessage(Stop{1}));
        CHECK(items[1].message && *items[1].message == ServerMessage(Stop{2}));
        CHECK(!items[2].message && contains(items[2].error, "not JSON"));
    }
}

// ---- the secret dialog's kinds (docs/secrets.md) ----

void secrets() {
    // The server's features: each a string; an older driver skips the kind (forward, above).
    const Decoded<ServerMessage> info = decodeServerMessage("{\"kind\":\"server\",\"features\":[\"secrets\",\"later\"]}");
    CHECK(info.message && *info.message == ServerMessage(ServerInfo{{"secrets", "later"}}));
    CHECK(!serverError("{\"kind\":\"server\",\"features\":[\"secrets\",1]}").empty());
    CHECK(!serverError("{\"kind\":\"server\",\"features\":\"secrets\"}").empty());
    CHECK(!serverError("{\"kind\":\"server\"}").empty());

    // The answer: stored, or refused with a reason; any other outcome, or a refusal without its reason, is refused.
    const Decoded<ServerMessage> stored = decodeServerMessage("{\"kind\":\"secret\",\"ask\":4,\"outcome\":\"stored\"}");
    CHECK(stored.message && *stored.message == ServerMessage(SecretAnswer{4, true, ""}));
    // Stored with a reason: the value is in its file, its durability unconfirmed.
    const Decoded<ServerMessage> unconfirmed =
        decodeServerMessage("{\"kind\":\"secret\",\"ask\":4,\"outcome\":\"stored\",\"reason\":\"unconfirmed\"}");
    CHECK(unconfirmed.message && *unconfirmed.message == ServerMessage(SecretAnswer{4, true, "unconfirmed"}));
    const Decoded<ServerMessage> refused =
        decodeServerMessage("{\"kind\":\"secret\",\"ask\":4,\"outcome\":\"refused\",\"reason\":\"timeout\"}");
    CHECK(refused.message && *refused.message == ServerMessage(SecretAnswer{4, false, "timeout"}));
    CHECK(contains(serverError("{\"kind\":\"secret\",\"ask\":4,\"outcome\":\"maybe\"}"), "stored, refused"));
    CHECK(!serverError("{\"kind\":\"secret\",\"ask\":4,\"outcome\":\"refused\"}").empty());
    CHECK(!serverError("{\"kind\":\"secret\",\"ask\":0,\"outcome\":\"stored\"}").empty());
    CHECK(!serverError("{\"kind\":\"secret\",\"outcome\":\"stored\"}").empty());

    // The ask: a number from 1, an account and a file, and a reason of one line.
    CHECK(contains(driverError("{\"kind\":\"ask-secret\",\"ask\":1,\"account\":\"a\",\"what\":\"b\",\"why\":\"x\\ny\"}"),
                   "one line"));
    CHECK(contains(driverError("{\"kind\":\"ask-secret\",\"ask\":1,\"account\":\"a\",\"what\":\"b\",\"why\":\"x\\ty\"}"),
                   "one line"));
    CHECK(!driverError("{\"kind\":\"ask-secret\",\"ask\":1,\"account\":\"\",\"what\":\"b\",\"why\":\"\"}").empty());
    CHECK(!driverError("{\"kind\":\"ask-secret\",\"ask\":1,\"account\":\"a\",\"what\":\"\",\"why\":\"\"}").empty());
    CHECK(!driverError("{\"kind\":\"ask-secret\",\"ask\":0,\"account\":\"a\",\"what\":\"b\",\"why\":\"\"}").empty());
    CHECK(!driverError("{\"kind\":\"ask-secret\",\"ask\":1,\"account\":\"a\",\"what\":\"b\"}").empty());
    // The encoder refuses what the decoder refuses.
    CHECK(!encodeError(AskSecret{1, "a", "b", "x\ny"}).empty());
    CHECK(!encodeError(AskSecret{0, "a", "b", ""}).empty());
}

// ---- the line limit ----

// The bytes of a text message's line, without its newline, besides its delta.
const std::size_t kTextBase = encode(DriverMessage{Text{1, ""}}).line.size() - 1;

// A text message whose line, without its newline, is `size` bytes, up to the limit.
std::string textLine(std::size_t size) {
    std::string line = encode(DriverMessage{Text{1, std::string(size - kTextBase, 'x')}}).line;
    line.pop_back();
    return line;
}

void limit() {
    const std::string under = textLine(kMaxLineBytes - 1);
    const std::string at = textLine(kMaxLineBytes);
    CHECK(under.size() == kMaxLineBytes - 1);
    CHECK(at.size() == kMaxLineBytes);
    // The encoder writes a line of exactly the limit and refuses one byte more.
    CHECK(contains(encodeError(Text{1, std::string(kMaxLineBytes - kTextBase + 1, 'x')}), "past the protocol's line"));

    // At the limit and one under: read. One over (encode refuses it, so it is made by hand): refused, with the line
    // after it read as usual. Fed whole and a byte at a time, which passes the limit in the middle of a feed and across
    // feeds.
    const std::string over = std::string(at).insert(at.size() - 2, "x");
    CHECK(over.size() == kMaxLineBytes + 1);
    const std::string next = encode(DriverMessage{TextEnd{1}}).line;
    const std::string s = under + "\n" + at + "\n" + over + "\n" + next;
    for (const bool bytewise : {false, true}) {
        DriverReader reader;
        std::vector<DriverReader::Item> items;
        if (bytewise) {
            for (char c : s)
                reader.feed(std::string_view(&c, 1), items);
        } else {
            reader.feed(s, items);
        }
        reader.finish(items);
        CHECK(items.size() == 4);
        if (items.size() != 4)
            continue;
        CHECK(items[0].message && std::get<Text>(*items[0].message).delta.size() == kMaxLineBytes - 1 - kTextBase);
        CHECK(items[1].message && std::get<Text>(*items[1].message).delta.size() == kMaxLineBytes - kTextBase);
        CHECK(!items[2].message && contains(items[2].error, "line 3 is longer than the protocol's limit of 1048576"));
        CHECK(items[3].message && *items[3].message == DriverMessage(TextEnd{1}));
    }

    // A CR before the newline counts toward the limit.
    LineReader crlf;
    std::vector<LineReader::Line> cr;
    crlf.feed(under + "\r\n" + at + "\r\n", cr);
    CHECK(cr.size() == 2 && cr[0].error.empty() && cr[0].text == under && contains(cr[1].error, "line 2 is longer"));

    // A line over the limit with no newline yet is refused as soon as it passes the limit, and nothing more of it is
    // held: what follows its newline is read.
    LineReader lines;
    std::vector<LineReader::Line> out;
    lines.feed(std::string(kMaxLineBytes + 1, 'z'), out);
    CHECK(out.size() == 1 && contains(out[0].error, "line 1 is longer"));
    lines.feed(std::string(3 * kMaxLineBytes, 'z'), out);
    CHECK(out.size() == 1);
    lines.feed("z\nok\n", out);
    lines.finish(out);
    CHECK(out.size() == 2 && out[1].error.empty() && out[1].text == "ok" && out[1].number == 2);
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"roundtrip", roundtrip},
    {"encode_refusals", encode_refusals},
    {"property", property},
    {"chunking", chunking},
    {"refusals", refusals},
    {"strings", strings},
    {"numbers", numbers},
    {"forward", forward},
    {"secrets", secrets},
    {"limit", limit},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : kGroups) {
        if (argc > 1 && std::strcmp(argv[1], g.name) != 0)
            continue;
        g.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "driver_protocol_test: no group %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "driver_protocol_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("driver_protocol_test: ok\n");
    return 0;
}
