#include "voice_turn.h"

#include <algorithm>
#include <utility>

namespace nexus::watch {

#ifdef NEXUS_WATCH_FAULTS
std::string gWatchFault;
#define FAULT(name) (gWatchFault == (name))
#else
#define FAULT(name) false
#endif

const char* phaseName(Phase phase) {
    switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::Listening: return "listening";
    case Phase::Transcribing: return "transcribing";
    case Phase::Choosing: return "choosing";
    case Phase::Sending: return "sending";
    case Phase::Waiting: return "waiting";
    case Phase::Speaking: return "speaking";
    case Phase::Interrupted: return "interrupted";
    case Phase::Disconnected: return "disconnected";
    }
    return "idle";
}

VoiceTurn::VoiceTurn(std::uint64_t nextPrompt, bool connected, std::uint64_t utterances)
    : phase_(connected ? Phase::Idle : Phase::Disconnected), connected_(connected), utterance_(utterances),
      nextPrompt_(nextPrompt == 0 ? 1 : nextPrompt) {}

void VoiceTurn::notice(std::string text) {
    ++notices_;
    says_.push_back(Say{Say::Kind::Notice, std::move(text), {}});
}

void VoiceTurn::cue(std::string text) {
    ++cues_;
    says_.push_back(Say{Say::Kind::Cue, std::move(text), {}});
}

Phase VoiceTurn::rest() const {
    if (!connected_)
        return Phase::Disconnected;
    if (!unapplied_.empty())
        return Phase::Sending;
    return awaiting_ != 0 ? Phase::Waiting : Phase::Idle;
}

void VoiceTurn::settle() {
    if ((unlocked_ || FAULT("speak-locked")) && (phase_ == Phase::Idle || phase_ == Phase::Waiting || phase_ == Phase::Sending) && !replies_.empty())
        phase_ = Phase::Speaking;
}

bool VoiceTurn::press(Ms now, bool micAllowed) {
    switch (phase_) {
    case Phase::Disconnected:
        // Why, as the phone said it (its lock, the server unreachable), when it has said.
        notice(why_.empty() || why_ == kNoWordYet
                   ? std::string("No server: your phone has no link to nexus. Open nexus on the phone, then try again.")
                   : "No server: " + why_);
        return false;
    case Phase::Speaking:
        phase_ = Phase::Interrupted;
        return true;
    case Phase::Idle:
    case Phase::Waiting:
    case Phase::Interrupted:
        if (!unlocked_ && !FAULT("listen-locked")) {
            notice("nexus is locked. Unlock the watch, on your wrist, to talk to your AIs.");
            return false;
        }
        if (!micAllowed) {
            if (FAULT("silent-mic"))
                return false;
            notice("The microphone is off for nexus. Allow it in the watch's Settings, Privacy, Microphone.");
            return false;
        }
        phase_ = Phase::Listening;
        listenedAt_ = now;
        return true;
    default:
        return false;
    }
}

bool VoiceTurn::release(Ms now) {
    if (phase_ != Phase::Listening)
        return false;
    if (now < listenedAt_ + kMinTalkMs) {
        notice("Too short to hear. Hold the button while you talk.");
        phase_ = rest();
        settle();
        return true;
    }
    heard_ = ++utterance_;
    deadline_ = now + kTranscribeMs;
    phase_ = Phase::Transcribing;
    return true;
}

bool VoiceTurn::transcript(std::uint64_t utterance, const std::string& text) {
    if (phase_ != Phase::Transcribing || utterance != heard_)
        return false;
    words_ = text;
    phase_ = Phase::Choosing;
    return true;
}

bool VoiceTurn::transcriptFailed(std::uint64_t utterance, const std::string& why) {
    if (phase_ != Phase::Transcribing || utterance != heard_)
        return false;
    notice("I didn't catch that: " + why);
    phase_ = rest();
    settle();
    return true;
}

std::optional<Prompt> VoiceTurn::choose() {
    if (phase_ != Phase::Choosing)
        return std::nullopt;
    match_ = matchAi(words_, ais_);
    std::optional<Prompt> out;
    const auto send = [&](const std::string& to, const std::string& text) {
        out = Prompt{nextPrompt_++, to, text};
        unapplied_.push_back(out->num);
        current_ = to;
        cue("To " + to + ".");
    };
    switch (match_.kind) {
    case AiMatch::Kind::Named: {
        const std::string& name = ais_[match_.index];
        if (match_.rest.empty()) {
            notice("What should I tell " + name + "? Hold and say it after the name.");
        } else {
            send(name, match_.rest);
        }
        break;
    }
    case AiMatch::Kind::Ambiguous: {
        std::string names;
        for (std::size_t k = 0; k < match_.candidates.size(); ++k)
            names += (k == 0 ? "" : k + 1 == match_.candidates.size() ? " and " : ", ") + ais_[match_.candidates[k]];
        notice("That name fits " + names + ". Say the whole name.");
        break;
    }
    case AiMatch::Kind::None:
        if (match_.rest.empty())
            notice("No words came through. Hold the button and try again.");
        else if (ais_.empty())
            notice("The server has not said which AIs there are yet. Try again in a moment.");
        else
            send(ais_.front(), match_.rest);   // no name: the first AI in the server's list
        break;
    }
    phase_ = rest();
    settle();
    return out;
}

bool VoiceTurn::applied(std::uint64_t upTo) {
    if (unapplied_.empty() || unapplied_.front() > upTo)
        return false;
    while (!unapplied_.empty() && unapplied_.front() <= upTo) {
        unapplied_.erase(unapplied_.begin());
        ++awaiting_;
    }
    if (phase_ == Phase::Sending)
        phase_ = rest();
    settle();
    return true;
}

bool VoiceTurn::lostPrompt(std::uint64_t num) {
    const auto it = std::find(unapplied_.begin(), unapplied_.end(), num);
    if (it == unapplied_.end())
        return false;
    unapplied_.erase(it);
    notice("Your last words may not have arrived: your phone restarted while sending them. Say them again.");
    if (phase_ == Phase::Sending)
        phase_ = rest();
    settle();
    return true;
}

void VoiceTurn::reply(const std::string& aiName, const std::string& text, bool isNotice) {
    replies_.push_back(Say{isNotice ? Say::Kind::Notice : Say::Kind::Reply, text, aiName});
    if (awaiting_ != 0)
        --awaiting_;
    if (phase_ == Phase::Waiting)
        phase_ = rest();
    settle();
}

bool VoiceTurn::speechDone() {
    if (phase_ != Phase::Speaking)
        return false;
    replies_.pop_front();
    phase_ = rest();
    if (!FAULT("unread"))
        settle();
    return true;
}

bool VoiceTurn::resume() {
    if (phase_ != Phase::Interrupted)
        return false;
    phase_ = rest();
    settle();
    return true;
}

bool VoiceTurn::dismiss() {
    if (phase_ != Phase::Interrupted)
        return false;
    replies_.clear();
    phase_ = rest();
    return true;
}

void VoiceTurn::lost(const std::string& why) {
    connected_ = false;
    why_ = why;
    if (phase_ == Phase::Disconnected)
        return;
    if (phase_ == Phase::Listening || phase_ == Phase::Transcribing || phase_ == Phase::Choosing)
        notice("Lost the server while you were talking (" + why + "). That was not sent.");
    else
        notice("Lost the server: " + why);
    phase_ = Phase::Disconnected;
}

bool VoiceTurn::back() {
    connected_ = true;
    if (phase_ != Phase::Disconnected)
        return false;
    cue("Connected.");
    if (FAULT("drop-on-back"))
        unapplied_.clear();
    phase_ = rest();
    settle();
    return true;
}

void VoiceTurn::lock() {
    unlocked_ = false;
    if (phase_ == Phase::Listening) {
        notice("The watch locked while you were talking. That was not sent.");
        phase_ = rest();
    } else if (phase_ == Phase::Speaking || phase_ == Phase::Interrupted) {
        notice("The watch locked. The reply waits until you unlock it.");
        phase_ = rest();
    }
}

void VoiceTurn::unlock() {
    unlocked_ = true;
    settle();
}

bool VoiceTurn::tick(Ms now) {
    if (phase_ != Phase::Transcribing || now < deadline_)
        return false;
    notice("No words came back in 15 seconds. Hold the button and try again.");
    phase_ = rest();
    settle();
    return true;
}

void VoiceTurn::setAis(std::vector<std::string> ais) {
    ais_ = std::move(ais);
}

} // namespace nexus::watch
