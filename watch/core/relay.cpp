#include "relay.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace nexus::watch {

#ifdef NEXUS_WATCH_FAULTS
#define FAULT(name) (gWatchFault == (name))
#else
#define FAULT(name) false
#endif

namespace {

// A frame is its magic, then fields: a number is its decimal digits and ';', a string its length in decimal, ':' and
// its bytes. Nothing is escaped, so any bytes (audio among them) go as they are.
constexpr std::string_view kWatchMagic = "nexus-watch ";
constexpr std::string_view kPhoneMagic = "nexus-phone ";
constexpr std::string_view kSaveMagic = "nexus-watch-outbox ";

struct Writer {
    std::string out;
    void u(std::uint64_t v) {
        out += std::to_string(v);
        out += ';';
    }
    void s(const std::string& v) {
        out += std::to_string(v.size());
        out += ':';
        out += v;
    }
};

struct Reader {
    const std::string& in;
    std::size_t at = 0;
    std::string why;

    bool fail(std::string w) {
        if (why.empty())
            why = std::move(w) + " at byte " + std::to_string(at);
        return false;
    }
    bool number(std::uint64_t* v, char end) {
        const char* b = in.data() + at;
        const char* e = in.data() + in.size();
        auto r = std::from_chars(b, e, *v);
        if (r.ec != std::errc() || r.ptr == b || r.ptr == e || *r.ptr != end)
            return fail("a number was expected");
        at = static_cast<std::size_t>(r.ptr - in.data()) + 1;
        return true;
    }
    bool u(std::uint64_t* v) { return number(v, ';'); }
    bool u8(std::uint8_t* v, std::uint8_t max) {
        std::uint64_t x = 0;
        if (!u(&x))
            return false;
        if (x > max)
            return fail("a kind past " + std::to_string(max));
        *v = static_cast<std::uint8_t>(x);
        return true;
    }
    bool s(std::string* v) {
        std::uint64_t n = 0;
        if (!number(&n, ':'))
            return false;
        if (n > in.size() - at)
            return fail("a string runs past the end");
        v->assign(in, at, n);
        at += n;
        return true;
    }
    // A count of items, each at least `least` bytes: refused when the bytes left cannot hold them.
    bool count(std::uint64_t* n, std::size_t least) {
        if (!u(n))
            return false;
        if (*n > (in.size() - at) / least)
            return fail("a count past what the frame holds");
        return true;
    }
    bool magic(std::string_view m) {
        if (in.compare(0, m.size(), m) != 0)
            return fail("not a frame of this kind");
        at = m.size();
        return true;
    }
    bool end() { return at == in.size() || fail("bytes after the frame's end"); }
};

void prompt(Writer& w, const Prompt& p) {
    w.u(p.num);
    w.s(p.aiName);
    w.s(p.text);
}

bool prompt(Reader& r, Prompt* p) {
    return r.u(&p->num) && r.s(&p->aiName) && r.s(&p->text);
}

bool proto(Reader& r, std::uint64_t* p) {
    if (!r.u(p))
        return false;
    return (*p >= kRelayProtoOldest && *p <= kRelayProto) || r.fail("protocol " + std::to_string(*p) + ", where this build speaks " +
                                       std::to_string(kRelayProto) + ": update nexus on the watch and the phone");
}

void switchOf(Writer& w, const std::optional<AccountSwitch>& a) {
    w.u(a ? 1 : 0);
    if (a) {
        w.u(a->num);
        w.s(a->aiName);
        w.s(a->account);
    }
}

bool switchOf(Reader& r, std::optional<AccountSwitch>* out) {
    std::uint64_t has = 0;
    if (!r.u(&has))
        return false;
    if (has > 1)
        return r.fail("an account switch flag past 1");
    if (has == 0)
        return true;
    AccountSwitch a;
    if (!(r.u(&a.num) && r.s(&a.aiName) && r.s(&a.account)))
        return false;
    if (a.num == 0)
        return r.fail("an account switch numbered 0");
    *out = std::move(a);
    return true;
}

void rosters(Writer& w, const std::vector<AiAccounts>& all) {
    w.u(all.size());
    for (const AiAccounts& ai : all) {
        w.s(ai.aiName);
        w.s(ai.current);
        w.u(ai.accounts.size());
        for (const AccountOption& o : ai.accounts) {
            w.s(o.name);
            w.s(o.label);
            w.s(o.email);
            w.u(o.signedIn ? 1 : 0);
        }
    }
}

bool rosters(Reader& r, std::vector<AiAccounts>* out) {
    std::uint64_t n = 0;
    bool ok = r.count(&n, 6);
    for (std::uint64_t k = 0; ok && k < n; ++k) {
        AiAccounts ai;
        std::uint64_t m = 0;
        ok = r.s(&ai.aiName) && r.s(&ai.current) && r.count(&m, 8);
        for (std::uint64_t j = 0; ok && j < m; ++j) {
            AccountOption o;
            std::uint64_t in = 0;
            ok = r.s(&o.name) && r.s(&o.label) && r.s(&o.email) && r.u(&in);
            if (ok && in > 1)
                ok = r.fail("a signed-in flag past 1");
            o.signedIn = in == 1;
            ai.accounts.push_back(std::move(o));
        }
        out->push_back(std::move(ai));
    }
    return ok;
}

} // namespace

std::vector<AccountRow> accountRows(const AiAccounts& ai, const std::optional<AccountSwitch>& held) {
    std::vector<AccountRow> rows;
    for (const AccountOption& o : ai.accounts) {
        if (!o.signedIn)
            continue;
        AccountRow row;
        row.account = o.name;
        row.shown = !o.email.empty() ? o.email : !o.label.empty() ? o.label : o.name;
        row.current = o.name == ai.current;
        row.switching = held && held->aiName == ai.aiName && held->account == o.name;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string encode(const WatchFrame& f) {
    Writer w;
    w.out = kWatchMagic;
    w.u(f.proto);
    w.s(f.watch);
    w.u(f.epoch);
    w.u(f.ack);
    w.u(f.prompts.size());
    for (const Prompt& p : f.prompts)
        prompt(w, p);
    w.u(f.audio ? 1 : 0);
    if (f.audio) {
        w.u(f.audio->utterance);
        w.s(f.audio->format);
        w.s(f.audio->bytes);
    }
    if (f.proto >= 2)
        switchOf(w, f.accountSwitch);
    return w.out;
}

std::string encode(const PhoneFrame& f) {
    Writer w;
    w.out = kPhoneMagic;
    w.u(f.proto);
    w.u(f.epoch);
    w.u(f.uncertain);
    w.u(f.applied);
    w.u(f.audioTaken);
    w.u(static_cast<std::uint64_t>(f.link));
    w.s(f.why);
    w.u(f.aisKnown ? 1 : 0);
    w.u(f.ais.size());
    for (const std::string& name : f.ais)
        w.s(name);
    w.u(f.events.size());
    for (const Event& e : f.events) {
        w.u(e.num);
        w.u(static_cast<std::uint64_t>(e.kind));
        w.u(e.utterance);
        w.s(e.aiName);
        w.s(e.text);
    }
    if (f.proto >= 2) {
        w.u(f.switched);
        rosters(w, f.rosters);
    }
    return w.out;
}

bool decode(const std::string& bytes, WatchFrame* out, std::string* why) {
    Reader r{bytes, 0, {}};
    WatchFrame f;
    std::uint64_t n = 0, audio = 0;
    bool ok = r.magic(kWatchMagic) && proto(r, &f.proto) && r.s(&f.watch) && r.u(&f.epoch) && r.u(&f.ack) &&
        r.count(&n, 6);
    for (std::uint64_t k = 0; ok && k < n; ++k) {
        Prompt p;
        ok = prompt(r, &p);
        f.prompts.push_back(std::move(p));
    }
    ok = ok && r.u(&audio);
    if (ok && audio > 1)
        ok = r.fail("an audio flag past 1");
    if (ok && audio == 1) {
        Audio a;
        ok = r.u(&a.utterance) && r.s(&a.format) && r.s(&a.bytes);
        f.audio = std::move(a);
    }
    // The account switch, from protocol 2: a frame of 1 ends before it.
    if (ok && f.proto >= 2)
        ok = switchOf(r, &f.accountSwitch);
    ok = ok && r.end();
    if (!ok) {
        if (why)
            *why = "a watch frame that does not read: " + r.why;
        return false;
    }
    *out = std::move(f);
    return true;
}

bool decode(const std::string& bytes, PhoneFrame* out, std::string* why) {
    Reader r{bytes, 0, {}};
    PhoneFrame f;
    std::uint8_t link = 0, kind = 0;
    std::uint64_t known = 0, n = 0;
    bool ok = r.magic(kPhoneMagic) && proto(r, &f.proto) && r.u(&f.epoch) && r.u(&f.uncertain) && r.u(&f.applied) &&
        r.u(&f.audioTaken) && r.u8(&link, 2) && r.s(&f.why) && r.u(&known) && r.count(&n, 2);
    if (ok && known > 1)
        ok = r.fail("an AIs flag past 1");
    f.link = static_cast<LinkPhase>(link);
    f.aisKnown = known == 1;
    for (std::uint64_t k = 0; ok && k < n; ++k) {
        std::string name;
        ok = r.s(&name);
        f.ais.push_back(std::move(name));
    }
    ok = ok && r.count(&n, 10);
    for (std::uint64_t k = 0; ok && k < n; ++k) {
        Event e;
        ok = r.u(&e.num) && r.u8(&kind, f.proto >= 2 ? 4 : 3) && r.u(&e.utterance) && r.s(&e.aiName) && r.s(&e.text);
        e.kind = static_cast<Event::Kind>(kind);
        f.events.push_back(std::move(e));
    }
    // The accounts, from protocol 2: a frame of 1 ends before them.
    if (ok && f.proto >= 2)
        ok = r.u(&f.switched) && rosters(r, &f.rosters);
    ok = ok && r.end();
    if (ok && f.uncertain > f.applied)
        ok = r.fail("prompts it cannot vouch for past those done with");
    if (!ok) {
        if (why)
            *why = "a phone frame that does not read: " + r.why;
        return false;
    }
    *out = std::move(f);
    return true;
}

// The watch's end.

WatchLink::WatchLink(std::string watchId) : id_(std::move(watchId)) {}

void WatchLink::add(Prompt p) {
    nextPrompt_ = std::max(nextPrompt_, p.num + 1);
    outbox_.push_back(std::move(p));
}

void WatchLink::record(Audio a) {
    utterances_ = std::max(utterances_, a.utterance);
    audio_ = std::move(a);
}

WatchFrame WatchLink::frame() const {
    WatchFrame f;
    f.watch = id_;
    f.epoch = epoch_;
    f.ack = ack_;
    f.prompts = outbox_;
    f.audio = audio_;
    f.accountSwitch = switch_;
    return f;
}

WatchLink::Taken WatchLink::take(const PhoneFrame& f) {
    Taken t;
    if (f.epoch != epoch_) {
        // A run of the phone's relay this watch has not heard from (the relay restarted, or this app did): where its
        // events start for this watch is the first one it still holds, since it drops only those acknowledged under
        // its epoch, by this app or the one before it.
        epoch_ = f.epoch;
        ack_ = 0;
        synced_ = false;
    }
    if (!synced_ && !f.events.empty()) {
        ack_ = f.events.front().num - 1;
        synced_ = true;
    }
    for (const Event& e : f.events) {
        if (e.num == ack_ + 1) {
            ack_ = e.num;
            t.events.push_back(e);
        }
    }
    // The ones the phone cannot vouch for first: they are at or below `applied` too, and are not applied.
    std::vector<Prompt> kept;
    for (Prompt& p : outbox_) {
        if (p.num <= f.uncertain)
            t.lost.push_back(std::move(p));
        else if (p.num > f.applied)
            kept.push_back(std::move(p));
    }
    outbox_ = std::move(kept);
    if (audio_ && audio_->utterance <= f.audioTaken)
        audio_.reset();
    if (switch_ && switch_->num <= f.switched)
        switch_.reset();
    rosters_ = f.rosters;
    t.applied = f.applied;
    t.link = f.link;
    t.why = f.why;
    t.aisKnown = f.aisKnown;
    t.ais = f.ais;
    return t;
}

bool WatchLink::choose(const std::string& ai, const std::string& account, std::string* why) {
    std::string unused;
    std::string& w = why ? *why : unused;
    w.clear();
    const auto roster = std::find_if(rosters_.begin(), rosters_.end(),
                                     [&](const AiAccounts& r) { return r.aiName == ai; });
    if (roster == rosters_.end()) {
        w = "your phone has not sent " + ai + "'s accounts yet";
        return false;
    }
    const auto option = std::find_if(roster->accounts.begin(), roster->accounts.end(),
                                     [&](const AccountOption& o) { return o.name == account; });
    if (option == roster->accounts.end() || !option->signedIn) {
        w = account + " is not signed in for " + ai + ". Sign it in on the phone first.";
        return false;
    }
    if (switch_ && switch_->aiName != ai) {
        w = "still switching " + switch_->aiName + "'s account. Try again in a moment.";
        return false;
    }
    const std::string& target = switch_ ? switch_->account : roster->current;
    if (account == target && !FAULT("switch-current"))
        return false;
    switch_ = AccountSwitch{nextSwitch_++, ai, account};
    return true;
}

std::vector<AccountRow> WatchLink::accounts(const std::string& ai) const {
    for (const AiAccounts& r : rosters_) {
        if (r.aiName == ai)
            return accountRows(r, switch_);
    }
    return {};
}

std::string WatchLink::save() const {
    Writer w;
    w.out = kSaveMagic;
    w.s(id_);
    w.u(nextPrompt_);
    w.u(utterances_);
    w.u(outbox_.size());
    for (const Prompt& p : outbox_)
        prompt(w, p);
    // Appended: the account switch's number and the switch held.
    w.u(nextSwitch_);
    switchOf(w, switch_);
    return w.out;
}

bool WatchLink::load(const std::string& bytes, WatchLink* out, std::string* why) {
    Reader r{bytes, 0, {}};
    WatchLink l;
    std::uint64_t n = 0;
    bool ok = r.magic(kSaveMagic) && r.s(&l.id_) && r.u(&l.nextPrompt_) && r.u(&l.utterances_) && r.count(&n, 6);
    for (std::uint64_t k = 0; ok && k < n; ++k) {
        Prompt p;
        ok = prompt(r, &p);
        if (ok && (p.num == 0 || p.num >= l.nextPrompt_ || (!l.outbox_.empty() && p.num <= l.outbox_.back().num)))
            ok = r.fail("an outbox out of order");
        l.outbox_.push_back(std::move(p));
    }
    // Saved before the account switch: none held, numbered from 1.
    if (ok && r.at < bytes.size()) {
        ok = r.u(&l.nextSwitch_) && switchOf(r, &l.switch_);
        if (ok && (l.nextSwitch_ == 0 || (l.switch_ && l.switch_->num >= l.nextSwitch_)))
            ok = r.fail("an account switch out of order");
    }
    ok = ok && r.end();
    if (ok && l.nextPrompt_ == 0)
        ok = r.fail("a next prompt of 0");
    if (!ok) {
        if (why)
            *why = "the watch's saved outbox does not read: " + r.why;
        return false;
    }
    *out = std::move(l);
    return true;
}

// The phone's end.

PhoneRelay::PhoneRelay(Persisted persisted, std::uint64_t epoch)
    : persisted_(std::move(persisted)), epoch_(epoch), bootHanded_(persisted_.handed), acked_(persisted_.handed) {
    if (FAULT("forget-handed")) {
        persisted_.handed = 0;
        bootHanded_ = 0;
        acked_ = 0;
    }
}

void PhoneRelay::event(Event::Kind kind, std::uint64_t utterance, std::string aiName, std::string text) {
    events_.push_back(Event{eventNext_++, kind, utterance, std::move(aiName), std::move(text)});
}

PhoneRelay::Exchanged PhoneRelay::exchange(const WatchFrame& in) {
    Exchanged x;
    if (in.watch != persisted_.watch) {
        // Another watch: its numbers are its own, from 1, the events' too (it has taken none). What was owed to the one
        // before goes with it.
        persisted_ = Persisted{in.watch, 0};
        bootHanded_ = 0;
        acked_ = 0;
        waiting_.clear();
        inflight_.clear();
        awaits_.clear();
        events_.clear();
        eventNext_ = 1;
        watchAck_ = 0;
        audioTaken_ = 0;
        switchSeen_ = 0;
        switchWaiting_.reset();
        switchInflight_.reset();
        switched_ = 0;
    }
    if (in.epoch == epoch_) {
        watchAck_ = std::max(watchAck_, in.ack);
        while (!events_.empty() && events_.front().num <= watchAck_)
            events_.pop_front();
    }
    if (!unlocked_) {
        x.reply = answer(in);
        return x;
    }
    for (const Prompt& p : in.prompts) {
        const std::uint64_t expected = persisted_.handed + waiting_.size() + 1;
        if (p.num == expected)
            waiting_.push_back(p);
    }
    if (in.audio && in.audio->utterance > audioTaken_) {
        audioTaken_ = in.audio->utterance;
        x.transcribe = in.audio;
    }
    // The newest switch only: one the watch made later replaces one still waiting here.
    if (in.accountSwitch && in.accountSwitch->num > switchSeen_) {
        switchSeen_ = in.accountSwitch->num;
        switchWaiting_ = in.accountSwitch;
    }
    x.reply = answer(in);
    return x;
}

PhoneFrame PhoneRelay::answer(const WatchFrame& in) const {
    PhoneFrame f;
    f.epoch = epoch_;
    f.applied = acked_;
    f.audioTaken = audioTaken_;
    f.link = last_.phase;
    f.why = last_.why;
    f.aisKnown = last_.aisKnown;
    f.ais = last_.ais;
    if (!f.aisKnown) {
        for (const LinkSession& s : last_.sessions) {
            if (std::find(f.ais.begin(), f.ais.end(), s.aiName) == f.ais.end())
                f.ais.push_back(s.aiName);
        }
    }
    const std::uint64_t after = in.epoch == epoch_ ? in.ack : 0;
    for (const Event& e : events_) {
        if (e.num > after)
            f.events.push_back(e);
    }
    f.uncertain = FAULT("vouch-all") ? 0 : bootHanded_;
    f.switched = switched_;
    f.rosters = last_.rosters;
    if (!unlocked_) {
        f.link = LinkPhase::Lost;
        f.why = kLockedWhy;
        f.aisKnown = false;
        f.ais.clear();
        f.events.clear();
        f.rosters.clear();
    }
    return f;
}

void PhoneRelay::transcribed(std::uint64_t utterance, bool ok, const std::string& text) {
    event(ok ? Event::Kind::Transcript : Event::Kind::TranscriptFailed, utterance, {}, text);
}

void PhoneRelay::readReplies(const LinkView& view) {
    for (auto it = awaits_.begin(); it != awaits_.end();) {
        const auto s = std::find_if(view.sessions.begin(), view.sessions.end(),
                                    [&](const LinkSession& x) { return x.id == it->first; });
        if (s == view.sessions.end()) {
            ++it;
            continue;
        }
        // The prompt's own entry, the newest Person entry since it was handed; the reply is what follows it.
        std::size_t person = s->entries.size();
        for (std::size_t k = 0; k < s->entries.size(); ++k) {
            if (s->first + k >= it->second.since && s->entries[k].kind == LinkEntry::Kind::Person)
                person = k;
        }
        const bool settled = s->activity == LinkSession::Activity::Idle || s->activity == LinkSession::Activity::Down;
        if (person == s->entries.size() || !settled || !s->entries.back().closed) {
            ++it;
            continue;
        }
        std::string reply, notice;
        for (std::size_t k = person + 1; k < s->entries.size(); ++k) {
            const LinkEntry& e = s->entries[k];
            if (e.kind == LinkEntry::Kind::Ai)
                reply += (reply.empty() ? "" : "\n") + e.text;
            else if (e.kind == LinkEntry::Kind::Notice)
                notice = e.text;
        }
        const std::string& name = it->second.aiName;
        if (!reply.empty())
            event(Event::Kind::Reply, 0, name, reply);
        else if (s->activity == LinkSession::Activity::Down)
            event(Event::Kind::Notice, 0, name, notice.empty() ? name + " can't run right now." : notice);
        else
            event(Event::Kind::Notice, 0, name, notice.empty() ? name + " finished without a reply." : notice);
        it = awaits_.erase(it);
    }
}

std::string PhoneRelay::currentAccount(const std::string& ai) const {
    for (const AiAccounts& r : last_.rosters) {
        if (r.aiName == ai)
            return r.current;
    }
    return {};
}

std::vector<LinkAction> PhoneRelay::link(const LinkView& view) {
    last_ = view;
    while (!inflight_.empty() && view.acknowledged && view.sent >= inflight_.front().second) {
        acked_ = std::max(acked_, inflight_.front().first);
        inflight_.pop_front();
    }
    if (switchInflight_ && view.acknowledged && view.sent >= switchInflight_->second) {
        switched_ = std::max(switched_, switchInflight_->first.num);
        switchInflight_.reset();
    }
    readReplies(view);
    std::vector<LinkAction> out;
    if (view.phase != LinkPhase::Connected || !unlocked_)
        return out;
    // The account switch first: the person chose it before anything they say next.
    if (switchWaiting_) {
        const AccountSwitch& s = *switchWaiting_;
        const LinkSession* session = nullptr;
        for (const LinkSession& x : view.sessions) {
            if (x.aiName == s.aiName && (session == nullptr || x.id < session->id))
                session = &x;
        }
        const auto roster = std::find_if(view.rosters.begin(), view.rosters.end(),
                                         [&](const AiAccounts& r) { return r.aiName == s.aiName; });
        if (roster != view.rosters.end()) {
            const auto option = std::find_if(roster->accounts.begin(), roster->accounts.end(),
                                             [&](const AccountOption& o) { return o.name == s.account; });
            if (option == roster->accounts.end() || !option->signedIn) {
                // Signed out (or gone) since the watch's list: done with, and the person is told, never left waiting.
                event(Event::Kind::AccountRefused, 0, s.aiName, s.account + " is not signed in for " + s.aiName +
                      " any more. Sign it in on the phone first. The account was not changed.");
                switched_ = std::max(switched_, s.num);
                switchWaiting_.reset();
            } else if (session == nullptr) {
                if (!ensured_[s.aiName]) {
                    ensured_[s.aiName] = true;
                    out.push_back(LinkAction{LinkAction::Kind::Ensure, 0, s.aiName, {}, 0});
                    ++linkSent_;
                }
            } else {
                out.push_back(LinkAction{LinkAction::Kind::SetAccount, session->id, s.aiName, s.account, s.num});
                ++linkSent_;
                switchInflight_.emplace(s, linkSent_);
                if (FAULT("switch-early"))
                    switched_ = std::max(switched_, s.num);
                switchWaiting_.reset();
            }
        }
    }
    while (!waiting_.empty()) {
        const Prompt& p = waiting_.front();
        const LinkSession* session = nullptr;
        for (const LinkSession& s : view.sessions) {
            if (s.aiName == p.aiName && (session == nullptr || s.id < session->id))
                session = &s;
        }
        if (session == nullptr) {
            if (view.aisKnown &&
                std::find(view.ais.begin(), view.ais.end(), p.aiName) == view.ais.end()) {
                // No such AI on the server: nothing to hand over, and the person is told, never left waiting.
                event(Event::Kind::Notice, 0, p.aiName, "There is no AI named " + p.aiName +
                      " on the server. Your words were not sent.");
                persisted_.handed = p.num;
                inflight_.emplace_back(p.num, linkSent_);
                waiting_.pop_front();
                continue;
            }
            if (!ensured_[p.aiName]) {
                ensured_[p.aiName] = true;
                out.push_back(LinkAction{LinkAction::Kind::Ensure, 0, p.aiName, {}, 0});
                ++linkSent_;
            }
            break;
        }
        out.push_back(LinkAction{LinkAction::Kind::Send, session->id, p.aiName, p.text, p.num});
        ++linkSent_;
        persisted_.handed = p.num;
        inflight_.emplace_back(p.num, linkSent_);
        awaits_.try_emplace(session->id, Await{p.aiName, session->total()});
        waiting_.pop_front();
    }
    // A prompt to no such AI is acknowledged with the ones before it.
    while (!inflight_.empty() && ((view.acknowledged && view.sent >= inflight_.front().second) || FAULT("ack-early"))) {
        acked_ = std::max(acked_, inflight_.front().first);
        inflight_.pop_front();
    }
    return out;
}

} // namespace nexus::watch
