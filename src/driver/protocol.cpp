#include "driver/protocol.h"

#include <cmath>
#include <type_traits>
#include <utility>

#include "pr/json.h"
#include "pr/utf8.h"

namespace nexus::driver {

namespace {

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

std::string_view view(PrStringView s) {
    return s.data != nullptr ? std::string_view(s.data, s.size) : std::string_view();
}

// The offset of the first byte of `text` that is not well-formed UTF-8, or npos. Stricter than prUtf8Validate, which
// passes an encoded surrogate: a sequence decodes as U+FFFD here only if it is ill-formed or is U+FFFD itself, and
// U+0000 (which prUtf8DecodeScalar also turns into U+FFFD) is left for the JSON reader to refuse.
std::size_t illFormedAt(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::uint32_t codePoint = 0;
        const std::uint32_t n = prUtf8DecodeScalar(pr(text), at, &codePoint);
        if (codePoint == kPrUtf8Replacement && text[at] != '\0' && text.substr(at, n) != "\xEF\xBF\xBD")
            return at;
        at += n;
    }
    return std::string_view::npos;
}

// ---- Writing ----

class Writer {
public:
    Writer() : w_(prJsonWriterCreate()) { prJsonWriterBeginObject(w_); }
    ~Writer() { prJsonWriterDestroy(w_); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    Writer& str(std::string_view key, std::string_view value) {
        prJsonWriterKey(w_, pr(key));
        prJsonWriterString(w_, pr(value));
        return *this;
    }
    Writer& num(std::string_view key, double value) {
        prJsonWriterKey(w_, pr(key));
        prJsonWriterNumber(w_, value);
        return *this;
    }
    Writer& turn(std::uint64_t turn) {
        if (turn == 0 || turn > kMaxTurn)
            fail("turn " + std::to_string(turn) + " is out of range; a turn is 1 to " + std::to_string(kMaxTurn));
        return num("turn", double(turn));
    }
    Writer& begin(std::string_view key) {
        prJsonWriterKey(w_, pr(key));
        prJsonWriterBeginObject(w_);
        return *this;
    }
    Writer& end() {
        prJsonWriterEndObject(w_);
        return *this;
    }
    void fail(std::string why) {
        if (error_.empty())
            error_ = std::move(why);
    }

    Encoded finish() {
        prJsonWriterEndObject(w_);
        Encoded e;
        if (!error_.empty()) {
            e.error = "cannot encode the message: " + error_;
            return e;
        }
        const std::string_view text = view(prJsonWriterText(w_));
        if (text.size() > kMaxLineBytes) {
            e.error = "the message is " + std::to_string(text.size()) + " bytes, past the protocol's line limit of " +
                std::to_string(kMaxLineBytes) + "; shorten it (a prompt this long must be split or attached as a file)";
            return e;
        }
        e.line.reserve(text.size() + 1);
        e.line.append(text);
        e.line.push_back('\n');
        return e;
    }

private:
    PrJsonWriter* w_;
    std::string error_;
};

struct ServerEncoder {
    Writer& w;
    void operator()(const Send& m) { w.str("kind", "send").turn(m.turn).str("prompt", m.prompt); }
    void operator()(const Stop&) { w.str("kind", "stop"); }
    void operator()(const Unknown& m) { w.str("kind", m.kind); }
};

struct DriverEncoder {
    Writer& w;
    void operator()(const Hello& m) {
        if (m.protocol < 1)
            w.fail("protocol version " + std::to_string(m.protocol) + " is out of range; versions start at 1");
        w.str("kind", "hello").num("protocol", m.protocol).str("harness", m.harness);
    }
    void operator()(const Credential& m) {
        w.str("kind", "credential").str("billing", billingName(m.billing));
        if (!m.plan.empty())
            w.str("plan", m.plan);
    }
    void operator()(const Session& m) { w.str("kind", "session").str("id", m.id); }
    void operator()(const Text& m) { w.str("kind", "text").turn(m.turn).str("delta", m.delta); }
    void operator()(const TextEnd& m) { w.str("kind", "text-end").turn(m.turn); }
    void operator()(const Tool& m) {
        if (m.summary.find_first_of("\r\n") != std::string::npos)
            w.fail("the tool's summary holds a line break; a summary is one line");
        w.str("kind", "tool").turn(m.turn).str("name", m.name).str("summary", m.summary);
    }
    void operator()(const Done& m) {
        w.str("kind", "done").turn(m.turn).str("outcome", outcomeName(m.outcome));
        if (m.sessionCostUsd) {
            if (!std::isfinite(*m.sessionCostUsd) || *m.sessionCostUsd < 0)
                w.fail("session_cost_usd must be a finite number of dollars, zero or more");
            w.num("session_cost_usd", *m.sessionCostUsd);
        }
        if (m.usage) {
            w.begin("usage");
            count("input_tokens", m.usage->input);
            count("output_tokens", m.usage->output);
            count("cache_read_tokens", m.usage->cacheRead);
            count("cache_write_tokens", m.usage->cacheWrite);
            w.end();
        }
    }
    void operator()(const Error& m) {
        w.str("kind", "error").str("cause", m.cause).str("fix", m.fix);
        if (m.turn)
            w.turn(*m.turn);
    }
    void operator()(const Unknown& m) { w.str("kind", m.kind); }

    void count(std::string_view key, const std::optional<std::uint64_t>& v) {
        if (!v)
            return;
        if (*v > kMaxInteger)
            w.fail(std::string(key) + " " + std::to_string(*v) + " is past what a JSON number holds exactly");
        w.num(key, double(*v));
    }
};

// ---- Reading ----

// The fields of one decoded object. Each getter records the first problem it meets; the decoder reads error() last.
class Fields {
public:
    Fields(const PrJson* doc, PrJsonValueIndex object, std::string_view kind)
        : doc_(doc), object_(object), kind_(kind) {}

    std::string str(std::string_view key, bool required, bool nonEmpty = false) {
        const PrJsonValueIndex v = member(key);
        if (v == kPrJsonMissing) {
            if (required)
                fail(std::string("has no \"") + std::string(key) + "\"");
            return {};
        }
        if (prJsonKind(doc_, v) != PrJsonKind_String) {
            fail(std::string("has \"") + std::string(key) + "\" that is not a string");
            return {};
        }
        std::string s(view(prJsonString(doc_, v)));
        if (nonEmpty && s.empty())
            fail(std::string("has an empty \"") + std::string(key) + "\"");
        return s;
    }

    // An integer from `lo` to `hi`.
    std::optional<std::uint64_t> integer(std::string_view key, bool required, std::uint64_t lo, std::uint64_t hi) {
        const PrJsonValueIndex v = member(key);
        if (v == kPrJsonMissing) {
            if (required)
                fail(std::string("has no \"") + std::string(key) + "\"");
            return std::nullopt;
        }
        const double d = prJsonNumber(doc_, v, -1);
        if (prJsonKind(doc_, v) != PrJsonKind_Number || d != std::floor(d) || d < double(lo) || d > double(hi)) {
            fail(std::string("has \"") + std::string(key) + "\" that is not an integer from " + std::to_string(lo) +
                " to " + std::to_string(hi));
            return std::nullopt;
        }
        return std::uint64_t(d);
    }

    std::uint64_t turn(bool required = true) { return integer("turn", required, 1, kMaxTurn).value_or(0); }

    std::optional<double> amount(std::string_view key) {
        const PrJsonValueIndex v = member(key);
        if (v == kPrJsonMissing)
            return std::nullopt;
        const double d = prJsonNumber(doc_, v, -1);
        if (prJsonKind(doc_, v) != PrJsonKind_Number || d < 0) {
            fail(std::string("has \"") + std::string(key) + "\" that is not a number, zero or more");
            return std::nullopt;
        }
        return d;
    }

    // A member that is an object, or kPrJsonMissing when it is absent.
    PrJsonValueIndex object(std::string_view key) {
        const PrJsonValueIndex v = member(key);
        if (v != kPrJsonMissing && prJsonKind(doc_, v) != PrJsonKind_Object) {
            fail(std::string("has \"") + std::string(key) + "\" that is not an object");
            return kPrJsonMissing;
        }
        return v;
    }

    // The fields of a member object; take() keeps the first problem they met.
    Fields in(PrJsonValueIndex object) const { return Fields(doc_, object, kind_); }
    void take(const Fields& inner) {
        if (error_.empty())
            error_ = inner.error_;
    }

    void fail(const std::string& what) {
        if (error_.empty())
            error_ = "a \"" + kind_ + "\" message " + what;
    }
    const std::string& error() const { return error_; }

private:
    // The member named `key`; an absent or null member is missing (an optional field may be written as null).
    PrJsonValueIndex member(std::string_view key) const {
        const PrJsonValueIndex v = prJsonMember(doc_, object_, pr(key));
        return v == kPrJsonMissing || prJsonKind(doc_, v) == PrJsonKind_Null ? kPrJsonMissing : v;
    }

    const PrJson* doc_;
    PrJsonValueIndex object_;
    std::string kind_;
    std::string error_;
};

// A parsed line: the document and its kind, or why it is not a message.
struct Parsed {
    PrJson* doc = prJsonCreate();
    std::string kind;
    std::string error;

    explicit Parsed(std::string_view line) {
        const std::size_t bad = illFormedAt(line);
        if (bad != std::string_view::npos) {
            error = "the line is not UTF-8 (an ill-formed byte sequence at byte " + std::to_string(bad) + "); the "
                "writer must send UTF-8";
            return;
        }
        std::size_t at = 0;
        if (!prJsonParse(doc, pr(line), &at)) {
            error = "the line is not JSON (it stops making sense at byte " + std::to_string(at) + "); the writer must "
                "send one JSON object per line";
            return;
        }
        if (prJsonKind(doc, 0) != PrJsonKind_Object) {
            error = "the line is JSON but not an object; each line is one object with a \"kind\"";
            return;
        }
        const PrJsonValueIndex k = prJsonMember(doc, 0, pr("kind"));
        if (prJsonKind(doc, k) != PrJsonKind_String || prJsonString(doc, k).size == 0) {
            error = "the object has no \"kind\" string; each line names its kind, as {\"kind\":\"send\",...}";
            return;
        }
        kind = std::string(view(prJsonString(doc, k)));
    }
    ~Parsed() { prJsonDestroy(doc); }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;

    Fields fields() const { return Fields(doc, 0, kind); }
};

template <class Message>
Decoded<Message> result(Message message, const Fields& f) {
    Decoded<Message> d;
    if (!f.error().empty())
        d.error = f.error() + "; see docs/driver-protocol.md for its fields";
    else
        d.message = std::move(message);
    return d;
}

template <class Message>
Decoded<Message> refused(std::string why) {
    Decoded<Message> d;
    d.error = std::move(why);
    return d;
}

} // namespace

std::string_view billingName(Billing billing) {
    switch (billing) {
        case Billing::Subscription: return "subscription";
        case Billing::ApiKey: return "api-key";
        case Billing::None: break;
    }
    return "none";
}

std::string_view outcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::Ok: return "ok";
        case Outcome::Stopped: return "stopped";
        case Outcome::Error: break;
    }
    return "error";
}

Encoded encode(const ServerMessage& message) {
    Writer w;
    std::visit(ServerEncoder{w}, message);
    return w.finish();
}

Encoded encode(const DriverMessage& message) {
    Writer w;
    std::visit(DriverEncoder{w}, message);
    return w.finish();
}

Decoded<ServerMessage> decodeServerMessage(std::string_view line) {
    const Parsed p(line);
    if (!p.error.empty())
        return refused<ServerMessage>(p.error);
    Fields f = p.fields();
    if (p.kind == "send") {
        Send m;
        m.turn = f.turn();
        m.prompt = f.str("prompt", true);
        return result<ServerMessage>(std::move(m), f);
    }
    if (p.kind == "stop")
        return result<ServerMessage>(Stop{}, f);
    return result<ServerMessage>(Unknown{p.kind}, f);
}

Decoded<DriverMessage> decodeDriverMessage(std::string_view line) {
    const Parsed p(line);
    if (!p.error.empty())
        return refused<DriverMessage>(p.error);
    Fields f = p.fields();
    const std::string& kind = p.kind;
    if (kind == "hello") {
        Hello m;
        m.protocol = int(f.integer("protocol", true, 1, 1u << 30).value_or(0));
        m.harness = f.str("harness", true, true);
        return result<DriverMessage>(std::move(m), f);
    }
    if (kind == "credential") {
        Credential m;
        const std::string billing = f.str("billing", true);
        if (billing == "subscription")
            m.billing = Billing::Subscription;
        else if (billing == "api-key")
            m.billing = Billing::ApiKey;
        else if (billing != "none")
            f.fail("has \"billing\" \"" + billing + "\"; it is one of subscription, api-key, none");
        m.plan = f.str("plan", false);
        return result<DriverMessage>(std::move(m), f);
    }
    if (kind == "session")
        return result<DriverMessage>(Session{f.str("id", true, true)}, f);
    if (kind == "text") {
        Text m;
        m.turn = f.turn();
        m.delta = f.str("delta", true);
        return result<DriverMessage>(std::move(m), f);
    }
    if (kind == "text-end")
        return result<DriverMessage>(TextEnd{f.turn()}, f);
    if (kind == "tool") {
        Tool m;
        m.turn = f.turn();
        m.name = f.str("name", true, true);
        m.summary = f.str("summary", true);
        if (m.summary.find_first_of("\r\n") != std::string::npos)
            f.fail("has a \"summary\" with a line break; a summary is one line");
        return result<DriverMessage>(std::move(m), f);
    }
    if (kind == "done") {
        Done m;
        m.turn = f.turn();
        const std::string outcome = f.str("outcome", true);
        if (outcome == "stopped")
            m.outcome = Outcome::Stopped;
        else if (outcome == "error")
            m.outcome = Outcome::Error;
        else if (outcome != "ok")
            f.fail("has \"outcome\" \"" + outcome + "\"; it is one of ok, stopped, error");
        m.sessionCostUsd = f.amount("session_cost_usd");
        const PrJsonValueIndex usage = f.object("usage");
        if (usage != kPrJsonMissing) {
            Fields u = f.in(usage);
            auto count = [&u](std::string_view key) { return u.integer(key, false, 0, kMaxInteger); };
            m.usage = Usage{count("input_tokens"), count("output_tokens"), count("cache_read_tokens"),
                count("cache_write_tokens")};
            f.take(u);
        }
        return result<DriverMessage>(std::move(m), f);
    }
    if (kind == "error") {
        Error m;
        m.cause = f.str("cause", true, true);
        m.fix = f.str("fix", true, true);
        const std::uint64_t turn = f.turn(false);
        if (turn != 0)
            m.turn = turn;
        return result<DriverMessage>(std::move(m), f);
    }
    return result<DriverMessage>(Unknown{kind}, f);
}

void LineReader::feed(std::string_view bytes, std::vector<Line>& out) {
    while (!bytes.empty()) {
        const std::size_t newline = bytes.find('\n');
        const std::string_view piece = bytes.substr(0, newline);
        if (!dropping_) {
            if (partial_.size() + piece.size() > kMaxLineBytes) {
                out.push_back(Line{{}, "line " + std::to_string(lineNumber_) + " is longer than the protocol's limit "
                    "of " + std::to_string(kMaxLineBytes) + " bytes and was dropped; the writer must keep each line "
                    "within it (stream long text as several deltas)", lineNumber_});
                partial_.clear();
                dropping_ = true;
            } else {
                partial_.append(piece);
            }
        }
        if (newline == std::string_view::npos)
            return;
        if (!dropping_)
            out.push_back(Line{std::move(partial_), {}, lineNumber_});
        partial_.clear();
        dropping_ = false;
        ++lineNumber_;
        bytes.remove_prefix(newline + 1);
    }
}

void LineReader::finish(std::vector<Line>& out) {
    if (!partial_.empty() && !dropping_)
        out.push_back(Line{{}, "the stream ended inside line " + std::to_string(lineNumber_) + " (" +
            std::to_string(partial_.size()) + " bytes with no newline); the writer must end every line with a "
            "newline", lineNumber_});
    partial_.clear();
    dropping_ = false;
}

template <class Message>
void MessageReader<Message>::decode(std::vector<Item>& out) {
    for (LineReader::Line& line : lines_) {
        if (!line.error.empty()) {
            out.push_back(Item{std::nullopt, std::move(line.error)});
            continue;
        }
        Item item;
        if constexpr (std::is_same_v<Message, ServerMessage>)
            item = decodeServerMessage(line.text);
        else
            item = decodeDriverMessage(line.text);
        if (!item.error.empty())
            item.error = "line " + std::to_string(line.number) + ": " + item.error;
        out.push_back(std::move(item));
    }
}

template class MessageReader<ServerMessage>;
template class MessageReader<DriverMessage>;

} // namespace nexus::driver
