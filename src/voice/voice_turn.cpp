#include "voice/voice_turn.h"

#include <utility>

namespace nexus::voice {
namespace {

// An AI's name as the strip titles it: "emma" is "Emma", "johnny-grok" is "Johnny-grok".
std::string titled(std::string_view name) {
    std::string out(name);
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = char(out[0] - 'a' + 'A');
    return out;
}

} // namespace

const char* phaseName(Phase phase) {
    switch (phase) {
        case Phase::Asking: return "asking";
        case Phase::Listening: return "listening";
        case Phase::Finishing: return "finishing";
        case Phase::Denied: return "denied";
        case Phase::Idle: break;
    }
    return "idle";
}

const char* replyName(Reply reply) {
    switch (reply) {
        case Reply::Text: return "text";
        case Reply::Voice: break;
    }
    return "voice";
}

Reply replyOn(bool menu) { return menu ? Reply::Voice : Reply::Text; }

void VoiceTurn::context(std::string current, std::vector<SpokenAI> ais) {
    current_ = std::move(current);
    ais_ = std::move(ais);
}

std::vector<Act> VoiceTurn::listen(double now) {
    std::vector<Act> acts;
    if (speaking_) acts.push_back({Act::Kind::StopSpeaking});
    speaking_ = false;
    queue_.clear(); // the person is talking over the replies: they stay in the transcript, unspoken
    acts.push_back({Act::Kind::StartListening});
    phase_ = Phase::Listening;
    heard_.clear();
    line_.clear();
    trouble_ = false;
    listenStart_ = now;
    return acts;
}

void VoiceTurn::speakNext(std::vector<Act>& acts) {
    if (speaking_ || queue_.empty() || phase_ == Phase::Listening || phase_ == Phase::Finishing ||
            phase_ == Phase::Asking)
        return;
    auto next = std::move(queue_.front());
    queue_.pop_front();
    speaking_ = true;
    acts.push_back({Act::Kind::Speak, std::move(next.aiName), std::move(next.text)});
}

std::vector<Act> VoiceTurn::press(double now) {
    switch (phase_) {
        case Phase::Idle:
        case Phase::Denied:
            if (permission_ == Permission::Granted) return listen(now);
            phase_ = Phase::Asking;
            listenStart_ = now; // a listen the answer starts counts from the press
            heard_.clear();     // what an earlier turn heard was sent or drafted already
            line_.clear();
            return {{Act::Kind::AskPermission}};
        case Phase::Listening:
            phase_ = Phase::Finishing;
            stoppedAt_ = now;
            return {{Act::Kind::StopListening}};
        case Phase::Asking:
        case Phase::Finishing:
            break;
    }
    return {};
}

std::vector<Act> VoiceTurn::permission(bool granted, std::string_view why) {
    if (phase_ != Phase::Asking) return {};
    if (granted) {
        permission_ = Permission::Granted;
        return listen(listenStart_);
    }
    permission_ = Permission::Refused;
    phase_ = Phase::Denied;
    trouble_ = true;
    line_ = why.empty() ? std::string("The microphone or speech recognition is not allowed for nexus. Allow both in "
        "Settings > Privacy & Security, then tap the microphone again.") : std::string(why);
    std::vector<Act> acts;
    speakNext(acts);
    return acts;
}

std::vector<Act> VoiceTurn::heard(std::string_view text, bool final) {
    if (phase_ != Phase::Listening && phase_ != Phase::Finishing) return {};
    heard_ = std::string(text);
    if (!final) return {};
    return finish();
}

std::vector<Act> VoiceTurn::finish() {
    std::vector<Act> acts;
    if (phase_ == Phase::Listening) acts.push_back({Act::Kind::StopListening});
    phase_ = Phase::Idle;
    const auto where = route(heard_, ais_);
    switch (where.route) {
        case Route::Named:
            acts.push_back({Act::Kind::Open, where.aiName});
            if (where.prompt.empty()) {
                line_ = titled(where.aiName);
            } else {
                acts.push_back({Act::Kind::Send, where.aiName, where.prompt});
                awaiting_.insert(where.aiName);
                line_ = "To " + titled(where.aiName);
            }
            break;
        case Route::Ambiguous: {
            std::string names;
            for (std::size_t i = 0; i < where.candidates.size(); ++i)
                names += (i == 0 ? "" : i + 1 == where.candidates.size() ? " or " : ", ") + titled(where.candidates[i]);
            acts.push_back({Act::Kind::Draft, current_, heard_});
            line_ = names + "? Nothing was sent: say the name again, or edit the draft and send it.";
            break;
        }
        case Route::Current: {
            // No name: the AI showing, or with none showing (the phone's list) the default AI, the first.
            const std::string to = !current_.empty() ? current_ : ais_.empty() ? std::string() : ais_.front().name;
            if (where.prompt.empty()) {
                line_ = "Heard nothing; nothing was sent.";
            } else if (to.empty()) {
                acts.push_back({Act::Kind::Draft, "", heard_});
                line_ = "No AI to send it to yet: the server sends them once it is reached. It is in the draft.";
            } else {
                acts.push_back({Act::Kind::Send, to, where.prompt});
                awaiting_.insert(to);
                line_ = "To " + titled(to);
            }
            break;
        }
    }
    speakNext(acts);
    return acts;
}

std::vector<Act> VoiceTurn::failed(std::string_view why) {
    if (phase_ != Phase::Listening && phase_ != Phase::Finishing && phase_ != Phase::Asking) return {};
    std::vector<Act> acts;
    if (phase_ == Phase::Listening) acts.push_back({Act::Kind::StopListening});
    phase_ = Phase::Idle;
    trouble_ = true;
    line_ = why.empty() ? std::string("Speech recognition stopped without saying why.") : std::string(why);
    if (!heard_.empty()) {
        acts.push_back({Act::Kind::Draft, current_, heard_});
        line_ += " What was heard is in the draft.";
    }
    speakNext(acts);
    return acts;
}

std::vector<Act> VoiceTurn::tick(double now) {
    if (phase_ == Phase::Listening && now - listenStart_ >= kMaxListenSeconds) {
        phase_ = Phase::Finishing;
        stoppedAt_ = now;
        return {{Act::Kind::StopListening}};
    }
    if (phase_ == Phase::Finishing && now - stoppedAt_ >= kFinishSeconds) return finish();
    return {};
}

std::vector<Act> VoiceTurn::replied(std::string_view aiName, std::string_view text) {
    if (reply_ == Reply::Text || text.empty() || awaiting_.find(aiName) == awaiting_.end()) return {};
    queue_.push_back({std::string(aiName), std::string(text)});
    std::vector<Act> acts;
    speakNext(acts);
    return acts;
}

std::vector<Act> VoiceTurn::spoken() {
    if (!speaking_) return {};
    speaking_ = false;
    std::vector<Act> acts;
    speakNext(acts);
    return acts;
}

void VoiceTurn::turnEnded(std::string_view aiName) {
    if (auto at = awaiting_.find(aiName); at != awaiting_.end()) awaiting_.erase(at);
}

std::vector<Act> VoiceTurn::setReply(Reply reply) {
    reply_ = reply;
    if (reply != Reply::Text) return {};
    queue_.clear();
    if (!speaking_) return {};
    speaking_ = false;
    return {{Act::Kind::StopSpeaking}};
}

} // namespace nexus::voice
