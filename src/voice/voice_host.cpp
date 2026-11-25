#include "voice/voice_host.h"

#include <cctype>

namespace nexus::voice {

void VoiceHost::carry(const std::vector<Act>& acts) {
    for (const auto& act : acts) {
        switch (act.kind) {
            case Act::Kind::AskPermission:
                if (speech_) speech_->askPermission();
                else inbox_.post({SpeechEvent::Kind::Permission, false,
                    "This build of nexus has no speech recognition, so it cannot listen. Type the prompt instead."});
                break;
            case Act::Kind::StartListening: if (speech_) speech_->startListening(); break;
            case Act::Kind::StopListening: if (speech_) speech_->stopListening(); break;
            case Act::Kind::Speak:
                if (speech_) speech_->speak(act.text);
                else inbox_.post({SpeechEvent::Kind::Spoken, true});
                break;
            case Act::Kind::StopSpeaking: if (speech_) speech_->stopSpeaking(); break;
            case Act::Kind::Open: if (surface_.open) surface_.open(act.aiName); break;
            case Act::Kind::Send: if (surface_.send) surface_.send(act.aiName, act.text); break;
            case Act::Kind::Draft: if (surface_.draft) surface_.draft(act.aiName, act.text); break;
        }
    }
}

void VoiceHost::frame(double now, std::string current, std::vector<SpokenAI> ais) {
    turn_.context(std::move(current), std::move(ais));
    for (auto& event : inbox_.drain()) {
        switch (event.kind) {
            case SpeechEvent::Kind::Permission: carry(turn_.permission(event.ok, event.text)); break;
            case SpeechEvent::Kind::Heard: carry(turn_.heard(event.text, event.ok)); break;
            case SpeechEvent::Kind::Failed: carry(turn_.failed(event.text)); break;
            case SpeechEvent::Kind::Spoken: carry(turn_.spoken()); break;
        }
    }
    carry(turn_.tick(now));
}

void VoiceHost::press(double now) {
    carry(turn_.press(now));
}

void VoiceHost::setReply(Reply reply) {
    carry(turn_.setReply(reply));
}

void VoiceHost::replied(const std::string& aiName, const std::string& text) {
    carry(turn_.replied(aiName, text));
}

std::string speakable(std::string_view text) {
    std::string out;
    bool fence = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == text.npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const auto first = line.find_first_not_of(" \t");
        std::string_view body = first == line.npos ? std::string_view{} : line.substr(first);
        if (body.starts_with("```") || body.starts_with("~~~")) {
            if (!fence) out += out.empty() || out.back() == '\n' ? "(code)\n" : "\n(code)\n";
            fence = !fence;
            continue;
        }
        if (fence) continue;
        while (!body.empty() && (body.front() == '#' || body.front() == '>')) body.remove_prefix(1);
        if (body.size() > 1 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ') body.remove_prefix(2);
        const auto lead = body.find_first_not_of(' ');
        body = lead == body.npos ? std::string_view{} : body.substr(lead);
        for (std::size_t i = 0; i < body.size(); ++i) {
            const char c = body[i];
            auto word = [&](std::size_t at) { return std::isalnum(static_cast<unsigned char>(body[at])) != 0; };
            const bool inWord = i > 0 && i + 1 < body.size() && word(i - 1) && word(i + 1); // snake_case keeps its '_'
            if (c == '*' || c == '`' || (c == '_' && !inWord)) continue;
            if (c == ']' && i + 1 < body.size() && body[i + 1] == '(') { // [words](address): the words only
                const auto close = body.find(')', i + 2);
                if (close != body.npos) {
                    i = close;
                    continue;
                }
            }
            if (c == '[') continue;
            out += c;
        }
        if (start <= text.size()) out += '\n';
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

} // namespace nexus::voice
