#include "relay/relay_wire.h"

#include <cstring>
#include <exception>

#include "sync/secrets.h"
#include "sync/wire.h"

namespace nexus::relay {

namespace {

constexpr char kMagic[] = "nexus-relay";
constexpr std::size_t kMagicSize = sizeof(kMagic) - 1;
constexpr std::size_t kHeader = 5;

void putU32(std::string* out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void putU64(std::string* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

// Reads little-endian numbers and byte strings off a body, remembering the first thing that was missing.
struct Cursor {
    const std::string& body;
    std::size_t at = 0;
    bool ok = true;

    std::uint64_t number(int bytes) {
        if (!ok || body.size() - at < static_cast<std::size_t>(bytes)) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i)
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(body[at + i])) << (8 * i);
        at += static_cast<std::size_t>(bytes);
        return v;
    }
    std::string bytes(std::size_t n) {
        if (!ok || body.size() - at < n) {
            ok = false;
            return {};
        }
        std::string s = body.substr(at, n);
        at += n;
        return s;
    }
    std::string rest() {
        std::string s = ok ? body.substr(at) : std::string();
        at = body.size();
        return s;
    }
};

std::string versionText(std::uint32_t version, std::uint32_t format) {
    return "relay version " + std::to_string(version) + ", state format " + std::to_string(format);
}

} // namespace

std::string frameBytes(FrameKind kind, const std::string& body) {
    std::string out;
    out.reserve(kHeader + body.size());
    out.push_back(static_cast<char>(kind));
    putU32(&out, static_cast<std::uint32_t>(body.size()));
    out += body;
    return out;
}

void FrameReader::feed(const char* data, std::size_t size) {
    if (failed()) return;
    // What earlier frames used goes before more is added, so the buffer holds at most one partial frame and the bytes
    // just read.
    if (at_ > 0) {
        volatile char* used = buffer_.data();
        for (std::size_t i = 0; i < at_; ++i) used[i] = 0;
        buffer_.erase(0, at_);
        at_ = 0;
    }
    buffer_.append(data, size);
}

bool FrameReader::next(Frame* out, std::string* why) {
    why->clear();
    if (failed()) {
        *why = failure_;
        return false;
    }
    if (buffer_.size() - at_ < kHeader) return false;
    const auto kind = static_cast<unsigned char>(buffer_[at_]);
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i)
        size |= static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[at_ + 1 + i])) << (8 * i);
    if (kind < static_cast<unsigned char>(FrameKind::Hello) || kind > static_cast<unsigned char>(FrameKind::History)) {
        failure_ = "a frame of kind " + std::to_string(kind) + ", which the relay protocol has not";
        *why = failure_;
        return false;
    }
    if (size > kMaxBody) {
        failure_ = "a frame of " + std::to_string(size) + " bytes, over the relay protocol's " +
            std::to_string(kMaxBody);
        *why = failure_;
        return false;
    }
    if (buffer_.size() - at_ - kHeader < size) return false;
    out->kind = static_cast<FrameKind>(kind);
    out->body.assign(buffer_, at_ + kHeader, size);
    at_ += kHeader + size;
    return true;
}

std::string helloFrame() {
    std::string body(kMagic, kMagicSize);
    putU32(&body, kRelayVersion);
    putU32(&body, sync::kStateFormat);
    return frameBytes(FrameKind::Hello, body);
}

bool checkHello(const std::string& body, const char* self, const char* other, std::string* why) {
    Cursor c{body};
    const std::string magic = c.bytes(kMagicSize);
    const auto version = static_cast<std::uint32_t>(c.number(4));
    const auto format = static_cast<std::uint32_t>(c.number(4));
    if (!c.ok || magic != std::string(kMagic, kMagicSize) || c.at != body.size()) {
        *why = std::string(other) + " sent a first frame that is not the relay's hello";
        return false;
    }
    if (version != kRelayVersion || format != sync::kStateFormat) {
        *why = std::string(other) + " speaks " + versionText(version, format) + ", and " + self + " " +
            versionText(kRelayVersion, sync::kStateFormat) + ": install the same nexus on Windows and in WSL2";
        return false;
    }
    return true;
}

std::string actionFrame(const sync::Action& action) {
    sync::Actions before(1);
    sync::Actions after(1);
    after.add(action);
    std::string body = after.diff_from(before);
    std::string frame = frameBytes(FrameKind::Action, body);
    wipe(&body);
    return frame;
}

bool readAction(const std::string& body, sync::Action* out, std::string* why) {
    std::vector<sync::Action> actions;
    try {
        actions = sync::Actions::parse(body, sync::kStateFormat);
    } catch (const std::exception& e) {
        *why = std::string("an action frame that does not parse: ") + e.what();
        return false;
    }
    if (actions.size() != 1) {
        *why = "an action frame holding " + std::to_string(actions.size()) + " actions, not one";
        return false;
    }
    *out = std::move(actions.front());
    return true;
}

std::string historyFrame(sync::SessionId id, std::uint64_t epoch, std::uint64_t before) {
    std::string body;
    putU64(&body, id);
    putU64(&body, epoch);
    putU64(&body, before);
    return frameBytes(FrameKind::History, body);
}

bool readHistory(const std::string& body, sync::SessionId* id, std::uint64_t* epoch, std::uint64_t* before,
                 std::string* why) {
    Cursor c{body};
    *id = c.number(8);
    *epoch = c.number(8);
    *before = c.number(8);
    if (!c.ok || c.at != body.size()) {
        *why = "a history frame of " + std::to_string(body.size()) + " bytes, not 24";
        return false;
    }
    return true;
}

// Body: phase (1), loopback (1), whole (1), format (4), heardMsAgo (8), the problem's length (4) and bytes, then the
// sessions' diff (whole: their init_diff).
std::string StateWriter::frame(const LinkState& state) {
    const bool whole = !sent_ || last_.format() != state.sessions.format();
    std::string body;
    body.push_back(static_cast<char>(state.phase));
    body.push_back(state.loopback ? 1 : 0);
    body.push_back(whole ? 1 : 0);
    putU32(&body, state.sessions.format());
    putU64(&body, state.heardMsAgo);
    putU32(&body, static_cast<std::uint32_t>(state.problem.size()));
    body += state.problem;
    body += whole ? state.sessions.init_diff() : state.sessions.diff_from(last_);
    last_ = state.sessions;
    sent_ = true;
    return frameBytes(FrameKind::State, body);
}

bool StateReader::apply(const std::string& body, std::string* why) {
    Cursor c{body};
    const auto phase = c.number(1);
    const auto loopback = c.number(1);
    const auto whole = c.number(1);
    const auto format = static_cast<std::uint32_t>(c.number(4));
    const auto heard = c.number(8);
    const auto problemSize = c.number(4);
    std::string problem = c.bytes(static_cast<std::size_t>(problemSize));
    const std::string diff = c.rest();
    if (!c.ok || phase > static_cast<std::uint64_t>(sync::LinkPhase::Failed) || loopback > 1 || whole > 1) {
        *why = "a state frame that does not parse";
        return false;
    }
    if (!whole && !whole_) {
        *why = "a state frame that is a diff, before any whole one";
        return false;
    }
    if (!whole && format != state_.sessions.format()) {
        *why = "a state frame that is a diff in format " + std::to_string(format) + " from sessions held in " +
            std::to_string(state_.sessions.format());
        return false;
    }
    sync::Sessions sessions = whole ? sync::Sessions() : state_.sessions;
    if (whole) sessions.setFormat(format);
    if (!diff.empty()) {
        try {
            sessions.apply_string(diff);
        } catch (const std::exception& e) {
            *why = std::string("a state frame whose sessions do not apply: ") + e.what();
            return false;
        }
    }
    state_.phase = static_cast<sync::LinkPhase>(phase);
    state_.loopback = loopback != 0;
    state_.heardMsAgo = heard;
    state_.problem = std::move(problem);
    state_.sessions = std::move(sessions);
    whole_ = true;
    return true;
}

void wipe(std::string* bytes) {
    sync::wipeBytes(*bytes);
}

} // namespace nexus::relay
