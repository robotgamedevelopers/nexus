#include "nexus_watch.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "relay.h"
#include "screen.h"
#include "voice_turn.h"

using namespace nexus::watch;

struct nw_client {
    WatchLink link;
    VoiceTurn turn;
    bool reached = false;       // the server reached since launch (screen.h's settle)
    uint64_t now = 0;           // the latest time the app gave
    std::string remembered = {};  // the first AI of the last launch's list (nw_remember_ai)
    bool aisKnown = false;      // the phone has sent the server's list
};

namespace {

char* dup(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p == nullptr)
        return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

size_t bytes(const std::string& s, char** out) {
    *out = static_cast<char*>(std::malloc(s.size() == 0 ? 1 : s.size()));
    if (*out == nullptr)
        return 0;
    std::memcpy(*out, s.data(), s.size());
    return s.size();
}

} // namespace

extern "C" {

nw_client* nw_client_new(const char* watch_id, const char* saved, size_t saved_len) {
    WatchLink link{watch_id ? watch_id : ""};
    std::string why;
    bool unread = false;
    if (saved != nullptr && saved_len != 0) {
        WatchLink loaded;
        if (WatchLink::load(std::string(saved, saved_len), &loaded, &why) && loaded.id() == link.id())
            link = std::move(loaded);
        else
            unread = true;
    }
    auto* c = new nw_client{std::move(link), VoiceTurn{1, false}};
    c->turn = VoiceTurn{c->link.nextPrompt(), false, c->link.utterances()};
    if (unread)
        c->turn.says().push_back(Say{Say::Kind::Notice,
            "The words this watch kept to send could not be read back (" + (why.empty() ? std::string("another watch's")
            : why) + "). Anything unsent from before is gone: say it again.", {}});
    return c;
}

void nw_client_free(nw_client* c) {
    delete c;
}

void nw_free(void* p) {
    std::free(p);
}

int nw_press(nw_client* c, uint64_t now_ms, int mic_allowed) {
    c->now = now_ms;
    return c->turn.press(now_ms, mic_allowed != 0) ? 1 : 0;
}

int nw_release(nw_client* c, uint64_t now_ms, const void* audio, size_t audio_len, const char* format) {
    c->now = now_ms;
    if (!c->turn.release(now_ms))
        return 0;
    if (c->turn.phase() == Phase::Transcribing) {
        c->link.record(Audio{c->turn.heard(), format ? format : "m4a",
                             std::string(static_cast<const char*>(audio), audio ? audio_len : 0)});
    }
    return 1;
}

int nw_speech_done(nw_client* c) {
    return c->turn.speechDone() ? 1 : 0;
}

int nw_resume(nw_client* c) {
    return c->turn.resume() ? 1 : 0;
}

int nw_dismiss(nw_client* c) {
    return c->turn.dismiss() ? 1 : 0;
}

int nw_tick(nw_client* c, uint64_t now_ms) {
    c->now = now_ms;
    return c->turn.tick(now_ms) ? 1 : 0;
}

void nw_lock(nw_client* c) {
    c->turn.lock();
}

void nw_unlock(nw_client* c) {
    c->turn.unlock();
}

int nw_unlocked(const nw_client* c) {
    return c->turn.unlocked() ? 1 : 0;
}

size_t nw_frame(nw_client* c, char** out) {
    return bytes(encode(c->link.frame()), out);
}

int nw_take(nw_client* c, const char* data, size_t len) {
    PhoneFrame f;
    std::string why;
    if (!decode(std::string(data, len), &f, &why)) {
        c->turn.lost("the phone's answer did not read: " + why);
        return 0;
    }
    WatchLink::Taken t = c->link.take(f);
    if (t.aisKnown || !t.ais.empty()) {
        c->turn.setAis(t.ais);
        c->aisKnown = true;
    }
    if (t.link == LinkPhase::Connected) {
        c->turn.back();
        c->reached = true;
    }
    else
        c->turn.lost(t.link == LinkPhase::Connecting ? "your phone is still reaching nexus-server" : t.why);
    for (const Prompt& p : t.lost)
        c->turn.lostPrompt(p.num);
    c->turn.applied(t.applied);
    for (const Event& e : t.events) {
        switch (e.kind) {
        case Event::Kind::Transcript:
            if (c->turn.transcript(e.utterance, e.text)) {
                if (std::optional<Prompt> p = c->turn.choose())
                    c->link.add(std::move(*p));
            }
            break;
        case Event::Kind::TranscriptFailed:
            c->turn.transcriptFailed(e.utterance, e.text);
            break;
        case Event::Kind::Reply:
            c->turn.reply(e.aiName, e.text, false);
            break;
        case Event::Kind::Notice:
            c->turn.reply(e.aiName, e.text, true);
            break;
        case Event::Kind::AccountRefused:
            c->turn.says().push_back(Say{Say::Kind::Notice, e.text, e.aiName});
            break;
        }
    }
    return 1;
}

void nw_carrier_failed(nw_client* c, const char* why) {
    c->turn.lost(why ? why : "the phone did not answer");
}

int nw_phase(const nw_client* c) {
    return static_cast<int>(c->turn.phase());
}

const char* nw_phase_name(int phase) {
    return phaseName(static_cast<Phase>(phase));
}

char* nw_current(const nw_client* c) {
    return dup(c->turn.current());
}

size_t nw_ai_count(const nw_client* c) {
    return c->turn.ais().size();
}

char* nw_ai(const nw_client* c, size_t i) {
    return dup(i < c->turn.ais().size() ? c->turn.ais()[i] : std::string());
}

char* nw_why(const nw_client* c) {
    return dup(c->turn.why());
}

char* nw_words(const nw_client* c) {
    return dup(c->turn.words());
}

size_t nw_outbox(const nw_client* c) {
    return c->link.outbox().size();
}

int nw_reply(const nw_client* c, int* kind, char** text, char** ai_name) {
    const Phase p = c->turn.phase();
    if ((p != Phase::Speaking && p != Phase::Interrupted) || c->turn.replies().empty())
        return 0;
    const Say& s = c->turn.replies().front();
    *kind = static_cast<int>(s.kind);
    *text = dup(s.text);
    *ai_name = dup(s.aiName);
    return 1;
}

int nw_next_say(nw_client* c, int* kind, char** text) {
    auto& says = c->turn.says();
    if (says.empty())
        return 0;
    *kind = static_cast<int>(says.front().kind);
    *text = dup(says.front().text);
    says.pop_front();
    return 1;
}

void nw_screen(const nw_client* c, const char* notice, const char* lock_why, const char* reply_from, char** status,
               char** problem) {
    ScreenIn in;
    in.phase = c->turn.phase();
    in.settled = c->reached || c->now >= kSettleMs;
    in.current = c->turn.current();
    in.replyFrom = reply_from ? reply_from : "";
    in.why = c->turn.why();
    in.notice = notice ? notice : "";
    in.lockWhy = lock_why ? lock_why : "";
    in.unlocked = c->turn.unlocked();
    const Screen s = screenOf(in);
    *status = dup(s.status);
    *problem = dup(s.problem);
}

size_t nw_saved(const nw_client* c, char** out) {
    return bytes(c->link.save(), out);
}

char* nw_shown_ai(const nw_client* c) {
    if (!c->turn.current().empty())
        return dup(c->turn.current());
    if (!c->aisKnown)
        return dup(c->remembered);
    return dup(c->turn.ais().empty() ? std::string() : c->turn.ais().front());
}

void nw_remember_ai(nw_client* c, const char* first_ai) {
    c->remembered = first_ai ? first_ai : "";
}

int nw_ais_known(const nw_client* c) {
    return c->aisKnown ? 1 : 0;
}

size_t nw_account_count(const nw_client* c, const char* ai) {
    return c->link.accounts(ai ? ai : "").size();
}

int nw_account(const nw_client* c, const char* ai, size_t i, char** account, char** shown, int* current,
               int* switching) {
    const std::vector<AccountRow> rows = c->link.accounts(ai ? ai : "");
    if (i >= rows.size())
        return 0;
    *account = dup(rows[i].account);
    *shown = dup(rows[i].shown);
    *current = rows[i].current ? 1 : 0;
    *switching = rows[i].switching ? 1 : 0;
    return 1;
}

int nw_choose_account(nw_client* c, const char* ai, const char* account) {
    const std::string name = ai ? ai : "";
    const std::string chosen = account ? account : "";
    std::string why;
    if (!c->link.choose(name, chosen, &why)) {
        if (!why.empty())
            c->turn.says().push_back(Say{Say::Kind::Notice, why, name});
        return 0;
    }
    std::string shown = chosen;
    for (const AccountRow& r : c->link.accounts(name)) {
        if (r.account == chosen)
            shown = r.shown;
    }
    c->turn.says().push_back(Say{Say::Kind::Cue, "Switching " + name + " to " + shown + ".", name});
    return 1;
}

} // extern "C"
