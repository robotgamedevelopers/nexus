// The phone's end of the watch's relay (watch/core/relay.h PhoneRelay) over nexus's own link (link/client.h), behind
// the C interface nexus_relay.h. What the relay must keep across runs (the watch it serves and the highest prompt it
// ever handed to a link) is written to its state file, durably, before the actions that advanced it are handed over.

#include "nexus_relay.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "link/client.h"
#include "relay.h"

using namespace nexus;
using namespace nexus::watch;

namespace {

char* dup(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p != nullptr) {
        std::memcpy(p, s.data(), s.size());
        p[s.size()] = '\0';
    }
    return p;
}

constexpr const char* kStateMagic = "nexus-watch-relay 1";

bool readState(const std::string& path, PhoneRelay::Persisted* out, std::string* why) {
    std::ifstream in(path);
    if (!in) {
        if (errno == ENOENT)
            return true;
        *why = "cannot read " + path + ": " + std::strerror(errno);
        return false;
    }
    std::string magic, watch;
    std::getline(in, magic);
    std::getline(in, watch);
    std::uint64_t handed = 0;
    if (magic != kStateMagic || !(in >> handed)) {
        *why = path + " is not the watch relay's state (its first line is not \"" + kStateMagic + "\"). Move it aside;"
            " the watch's words then sent before it may be reported as not vouched for.";
        return false;
    }
    *out = PhoneRelay::Persisted{watch, handed};
    return true;
}

// Written whole or not at all: a temporary file, flushed to the disk, renamed over the old one.
bool writeState(const std::string& path, const PhoneRelay::Persisted& p, std::string* why) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        *why = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    const std::string text = std::string(kStateMagic) + "\n" + p.watch + "\n" + std::to_string(p.handed) + "\n";
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
#ifdef F_FULLFSYNC
    ok = ok && ::fcntl(fd, F_FULLFSYNC) == 0;
#else
    ok = ok && ::fsync(fd) == 0;
#endif
    const int err = errno;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        *why = "cannot write " + path + ": " + std::strerror(ok ? errno : err);
        return false;
    }
    return true;
}

LinkView viewOf(const link::ClientLink::Snapshot& s) {
    LinkView v;
    if (s.phase == sync::LinkPhase::Connected && s.heardMsAgo <= sync::kQuietMs) {
        v.phase = LinkPhase::Connected;
    } else if (s.phase == sync::LinkPhase::Connected) {
        v.phase = LinkPhase::Lost;
        v.why = "nexus-server has not been heard for " + std::to_string(s.heardMsAgo / 1000) + " seconds";
    } else if (s.phase == sync::LinkPhase::Failed) {
        v.phase = LinkPhase::Lost;
        // Plain words first (the watch shows two lines); the link's own account after, for whoever reads on.
        v.why = "the phone could not reach nexus-server" + (s.problem.empty() ? std::string() : ". " + s.problem);
    }
    v.aisKnown = s.sessions.friendsKnown();   // the sync wire's names until its rename lands
    for (const sync::ServerFriend& f : s.sessions.friends())
        v.ais.push_back(f.name);
    for (const sync::SessionRef& ref : s.sessions.sessions()) {
        LinkSession ls;
        ls.id = ref->id;
        ls.aiName = ref->info.friendName;
        ls.activity = ref->activity == sync::Activity::Idle ? LinkSession::Activity::Idle
            : ref->activity == sync::Activity::Down        ? LinkSession::Activity::Down
            : ref->activity == sync::Activity::Working     ? LinkSession::Activity::Working
                                                           : LinkSession::Activity::Busy;
        ls.first = ref->first;
        for (const sync::EntryRef& e : ref->entries) {
            LinkEntry le;
            le.kind = e->kind == EntryKind::Person ? LinkEntry::Kind::Person
                : e->kind == EntryKind::Friend     ? LinkEntry::Kind::Ai
                : e->kind == EntryKind::Tool       ? LinkEntry::Kind::Tool
                : e->kind == EntryKind::Bus        ? LinkEntry::Kind::Bus
                                                   : LinkEntry::Kind::Notice;
            le.text = e->text;
            le.closed = e->closed;
            ls.entries.push_back(std::move(le));
        }
        v.sessions.push_back(std::move(ls));
    }
    // Each AI's accounts (sync::ServerFriend::accounts), and the one its first session runs on: the session the relay
    // sends its prompts and account switches to.
    for (const sync::ServerFriend& f : s.sessions.friends()) {
        AiAccounts ai;
        ai.aiName = f.name;
        for (const sync::AccountChoice& a : f.accounts)
            ai.accounts.push_back(AccountOption{a.name, a.label, a.email, !a.needsSignIn});
        const sync::SessionView* first = nullptr;
        for (const sync::SessionRef& ref : s.sessions.sessions()) {
            if (ref->info.friendName == f.name && (first == nullptr || ref->id < first->id))
                first = ref.get();
        }
        if (first != nullptr)
            ai.current = !first->info.account.empty() ? first->info.account
                : first->info.primaryAccount             ? first->info.primaryAccount->name
                                                         : std::string();
        v.rosters.push_back(std::move(ai));
    }
    v.sent = s.sent;
    v.acknowledged = s.acknowledged;
    return v;
}

sync::Action actionOf(const LinkAction& a) {
    sync::Action out;
    out.kind = a.kind == LinkAction::Kind::Ensure ? sync::ActionKind::Ensure
        : a.kind == LinkAction::Kind::SetAccount  ? sync::ActionKind::SetAccount
                                                  : sync::ActionKind::Send;
    out.session = a.session;
    out.friendName = a.kind == LinkAction::Kind::Ensure ? a.aiName : std::string();
    out.text = a.text;      // Send: the words; SetAccount: the account
    return out;
}

std::uint64_t randomEpoch() {
    std::random_device d;
    const std::uint64_t e = (static_cast<std::uint64_t>(d()) << 32) ^ d();
    return e == 0 ? 1 : e;
}

} // namespace

struct nr_host {
    std::mutex mutex;
    std::string statePath;
    PhoneRelay relay;
    PhoneRelay::Persisted written;
    std::vector<LinkAction> held;           // advanced in memory, waiting for the state file to take it
    link::ClientLink::Snapshot snapshot;
    LinkView view;
    std::unique_ptr<link::ClientLink> link;

    nr_host(std::string path, PhoneRelay::Persisted p) : statePath(std::move(path)), relay(p, randomEpoch()), written(p) {}

    // Under the mutex: the link's news to the relay, the relay's actions durably noted and then handed over.
    bool pump(std::string* why) {
        if (link->poll(&snapshot))
            view = viewOf(snapshot);
        // The link's own count includes what is held here and not yet handed: the relay must not count it acknowledged.
        LinkView v = view;
        v.acknowledged = v.acknowledged && held.empty();
        std::vector<LinkAction> more = relay.link(v);
        held.insert(held.end(), more.begin(), more.end());
        return flush(why);
    }

    bool flush(std::string* why) {
        if (!(relay.persisted() == written)) {
            if (!writeState(statePath, relay.persisted(), why))
                return false;
            written = relay.persisted();
        }
        for (const LinkAction& a : held)
            link->send(actionOf(a));
        held.clear();
        return true;
    }
};

extern "C" {

nr_host* nr_host_new(const char* host, const char* const* command, size_t command_count, const char* state_path,
                     char** why) {
    PhoneRelay::Persisted p;
    std::string problem;
    if (!readState(state_path ? state_path : "", &p, &problem)) {
        if (why)
            *why = dup(problem);
        return nullptr;
    }
    auto* h = new nr_host(state_path, p);
    link::ClientLink::Options o;
    o.bootstrap.host = host ? host : "localhost";
    if (command != nullptr && command_count != 0)
        o.bootstrap.command.assign(command, command + command_count);
    h->link = std::make_unique<link::ClientLink>(std::move(o));
    return h;
}

void nr_host_free(nr_host* h) {
    delete h;
}

void nr_free(void* p) {
    std::free(p);
}

size_t nr_host_exchange(nr_host* h, const char* frame, size_t len, char** reply, uint64_t* utterance, char** audio,
                        size_t* audio_len, char** format, char** why) {
    std::lock_guard<std::mutex> lock(h->mutex);
    *utterance = 0;
    WatchFrame in;
    std::string problem;
    if (!decode(std::string(frame, len), &in, &problem)) {
        if (why)
            *why = dup(problem);
        return 0;
    }
    PhoneRelay::Exchanged x = h->relay.exchange(in);
    // A new watch changes the state file: written before anything for it is handed over (pump's flush).
    if (!h->pump(&problem)) {
        if (why)
            *why = dup(problem);
        return 0;
    }
    x.reply = h->relay.answer(in);   // as of the link's latest, after the pump
    if (x.transcribe) {
        *utterance = x.transcribe->utterance;
        *audio = static_cast<char*>(std::malloc(x.transcribe->bytes.size() + 1));
        std::memcpy(*audio, x.transcribe->bytes.data(), x.transcribe->bytes.size());
        *audio_len = x.transcribe->bytes.size();
        *format = dup(x.transcribe->format);
    }
    const std::string out = encode(x.reply);
    *reply = static_cast<char*>(std::malloc(out.size()));
    std::memcpy(*reply, out.data(), out.size());
    return out.size();
}

void nr_host_transcribed(nr_host* h, uint64_t utterance, int ok, const char* text) {
    std::lock_guard<std::mutex> lock(h->mutex);
    h->relay.transcribed(utterance, ok != 0, text ? text : "");
}

int nr_host_pump(nr_host* h, char** why) {
    std::lock_guard<std::mutex> lock(h->mutex);
    std::string problem;
    if (!h->pump(&problem)) {
        if (why)
            *why = dup(problem);
        return 0;
    }
    return 1;
}

void nr_host_set_unlocked(nr_host* h, int unlocked) {
    std::lock_guard<std::mutex> lock(h->mutex);
    h->relay.setUnlocked(unlocked != 0);
}

int nr_host_link(nr_host* h) {
    std::lock_guard<std::mutex> lock(h->mutex);
    return static_cast<int>(h->view.phase);
}

} // extern "C"
