// Unit tests of the accounts and friends (src/accounts/registry.h) and of the hub's decisions about them
// (src/sync/hub.h, models/Accounts.tla): the files' layout and every refusal with its line, the version rule, the
// consistency rules, the accounts the server picks, and the hub's actions, sets, creates, reloads and rotations over a
// MemoryStore with a keeper that records what it is given. Logic only: no time, no sockets, no network, no files.
//
//   accounts_test            runs every group
//   accounts_test <group>    runs one

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "sync/hub.h"

namespace {

using namespace nexus;
using accounts::Registry;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool has(const std::vector<std::string>& lines, const std::string& part) {
    for (const std::string& l : lines) {
        if (l.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

Registry sample() {
    Registry r;
    r.accounts = {{"work", "claude"}, {"home", "claude"}, {"cx", "codex"}};
    r.friends = {{"ada", {"work", "home", "cx"}, "work"}, {"bo", {"home"}, ""}};
    return r;
}

// A version 1 accounts file as a person might write it: other spacing, keys in another order.
const char* const kAccountsV1 =
    "{\"accounts\": [{\"provider\": \"claude\", \"name\": \"work\"},\n"
    "  {\"name\": \"cx\", \"provider\": \"codex\"}],\n"
    " \"version\": 1}\n";

void layout() {
    const Registry r = sample();
    const std::string a = accounts::accountsText(r);
    const std::string f = accounts::friendsText(r);
    CHECK(a ==
        "{\n  \"version\": 1,\n  \"accounts\": [\n    {\n      \"name\": \"work\",\n      \"provider\": \"claude\"\n"
        "    },\n    {\n      \"name\": \"home\",\n      \"provider\": \"claude\"\n    },\n    {\n      \"name\": \"cx\",\n"
        "      \"provider\": \"codex\"\n    }\n  ]\n}\n");
    CHECK(f.find("\"default\": null") != std::string::npos);
    CHECK(f.back() == '\n');
    Registry back;
    CHECK(accounts::readAccounts(a, "accounts.json", &back).ok);
    CHECK(accounts::readFriends(f, "friends.json", &back).ok);
    CHECK(back == r);
    // Writing what was read gives the same text: a save never reshuffles the lists.
    CHECK(accounts::accountsText(back) == a);
    CHECK(accounts::friendsText(back) == f);
    Registry none;
    CHECK(accounts::accountsText(none) == "{\n  \"version\": 1,\n  \"accounts\": []\n}\n");
    CHECK(accounts::readAccounts(accounts::accountsText(none), "a", &back).ok && back.accounts.empty());
    // A string with a quote and a backslash survives.
    Registry odd;
    odd.friends = {{"x", {}, ""}};
    odd.accounts = {{"a", "claude"}};
    CHECK(accounts::readFriends(accounts::friendsText(odd), "f", &back).ok && back.friends == odd.friends);
}

void refusals() {
    Registry r;
    accounts::Read rd = accounts::readAccounts("{\n  \"version\": 1,\n  \"accounts\": [\n    {\"name\": \"w\",}\n  ]\n}\n",
                                               "/x/accounts.json", &r);
    CHECK(!rd.ok);
    CHECK(has(rd.errors, "/x/accounts.json:4: this is not JSON"));
    rd = accounts::readAccounts("{\n  \"version\": 1,\n  \"accounts\": [\n    {\"name\": \"w\", \"provder\": \"claude\"}\n"
                                "  ]\n}\n", "a.json", &r);
    CHECK(!rd.ok);
    CHECK(has(rd.errors, "a.json:4: an account has a field \"provder\""));
    CHECK(has(rd.errors, "has no \"provider\""));
    rd = accounts::readAccounts("{\"version\": 1, \"accounts\": [{\"name\": \".dot\", \"provider\": \"claude\"}]}", "a",
                                &r);
    CHECK(!rd.ok && has(rd.errors, "is not a name"));
    rd = accounts::readAccounts("{\"version\": 1, \"accounts\": [{\"name\": \"w\", \"provider\": \"Claude\"}]}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "provider \"Claude\" is not one"));
    rd = accounts::readAccounts("{\"version\": 1, \"accounts\": {}}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "\"accounts\" is an object, not a list"));
    rd = accounts::readAccounts("[]", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "the file is a list, not an object"));
    rd = accounts::readAccounts("{\"accounts\": []}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "has no \"version\""));
    rd = accounts::readFriends("{\"version\": 1, \"friends\": [{\"name\": \"ada\", \"accounts\": \"work\"}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "\"accounts\" is a string, not a list"));
    rd = accounts::readFriends("{\"version\": 1, \"friends\": [{\"name\": \"ada\", \"accounts\": [1]}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "holds a number, not an account's name"));
    rd = accounts::readFriends("{\"version\": 1, \"friends\": [{\"name\": \"ada\", \"default\": 3}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "\"default\" is a number"));
    rd = accounts::readFriends("{\"version\": 1, \"friends\": [{\"name\": \"ada\", \"name\": \"b\"}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "has the field \"name\" twice"));
    // Every error is found, not only the first.
    rd = accounts::readAccounts("{\"version\": 1, \"accounts\": [{\"name\": 1, \"provider\": \"claude\"}, "
                                "{\"name\": \"b\", \"provider\": 2}]}", "a", &r);
    CHECK(rd.errors.size() == 2);
    // An empty or missing file is none.
    CHECK(accounts::readFriends("", "f", &r).ok && r.friends.empty());
    CHECK(accounts::readFriends(" \n", "f", &r).ok);
}

void versions() {
    Registry r;
    // The version 1 fixture, written by hand, reads; it is the newest, so nothing upgrades.
    accounts::Read rd = accounts::readAccounts(kAccountsV1, "a", &r);
    CHECK(rd.ok && rd.version == 1 && !rd.upgraded);
    CHECK(r.accounts.size() == 2 && r.accounts[0].name == "work" && r.accounts[1].provider == "codex");
    // Written back in the one layout, in the person's order.
    CHECK(accounts::accountsText(r).find("\"name\": \"work\"") < accounts::accountsText(r).find("\"name\": \"cx\""));
    rd = accounts::readAccounts("{\"version\": 2, \"accounts\": []}", "/h/accounts.json", &r);
    CHECK(!rd.ok && has(rd.errors, "/h/accounts.json:1: \"version\" is 2, newer than this nexus-server reads (1)"));
    CHECK(has(rd.errors, "the file was left as it is"));
    rd = accounts::readAccounts("{\"version\": 1.5, \"accounts\": []}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "not a whole number"));
    rd = accounts::readAccounts("{\"version\": 0, \"accounts\": []}", "a", &r);
    CHECK(!rd.ok);
}

void consistency() {
    CHECK(accounts::problems(sample()).empty());
    Registry r = sample();
    r.friends[0].accounts.push_back("gone");
    CHECK(has(accounts::problems(r), "links to the account \"gone\", which accounts.json does not set up"));
    r = sample();
    r.friends[1].preferred = "work";
    CHECK(has(accounts::problems(r), "\"default\" is \"work\", which is not in its \"accounts\""));
    r = sample();
    r.accounts.push_back({"work", "claude"});
    CHECK(has(accounts::problems(r), "the account \"work\" is there twice"));
    r = sample();
    r.friends[0].accounts.push_back("work");
    CHECK(has(accounts::problems(r), "links to the account \"work\" twice"));
    r = sample();
    r.friends.push_back({"bo", {}, ""});
    CHECK(has(accounts::problems(r), "the friend \"bo\" is there twice"));
}

void picks() {
    const Registry r = sample();
    std::set<std::string> up = {"home", "cx"};
    // In link order after the current one, cycling round, never the current one.
    CHECK(accounts::nextUp(r, up, "ada", "work") == "home");
    CHECK(accounts::nextUp(r, up, "ada", "home") == "cx");
    CHECK(accounts::nextUp(r, up, "ada", "cx") == "home");
    CHECK(accounts::nextUp(r, {"work"}, "ada", "work").empty());
    CHECK(accounts::nextUp(r, up, "ada", "").empty() == false);
    CHECK(accounts::nextUp(r, up, "ada", "") == "home");
    CHECK(accounts::nextUp(r, {}, "ada", "work").empty());
    CHECK(accounts::nextUp(r, up, "nobody", "").empty());
    // The default when signed in, else the first signed-in link, else none.
    CHECK(accounts::landing(r, {"work", "cx"}, "ada") == "work");
    CHECK(accounts::landing(r, up, "ada") == "home");
    CHECK(accounts::landing(r, {}, "ada").empty());
    CHECK(accounts::landing(r, up, "bo") == "home");
    const std::vector<std::string> c = accounts::changes(Registry{}, r);
    CHECK(has(c, "account work set up (claude)") && has(c, "friend ada set up, accounts [work, home, cx], default work"));
    Registry less = r;
    less.friends[0].accounts.pop_back();
    less.accounts.pop_back();
    CHECK(has(accounts::changes(r, less), "account cx removed"));
    CHECK(has(accounts::changes(r, less), "friend ada's accounts [work, home, cx] -> [work, home]"));
}

// The keeper: records what it is given; refuses while `refuse` is set.
class Keeper : public sync::AccountsKeeper {
public:
    bool keep(const Registry& r, const std::string& message, std::string* why) override {
        if (!refuse.empty()) {
            *why = refuse;
            return false;
        }
        kept.push_back(r);
        messages.push_back(message);
        return true;
    }
    std::vector<Registry> kept;
    std::vector<std::string> messages;
    std::string refuse;
};

sync::Action act(std::uint64_t num, sync::ActionKind kind, std::string friendName, std::string account,
                 std::string text = std::string(), sync::SessionId session = 0) {
    return sync::Action{num, kind, session, std::move(friendName), std::move(text), std::move(account)};
}

std::string lastNotice(const sync::SessionStore& store, sync::SessionId id) {
    const sync::StoredSession* s = store.find(id);
    for (auto it = s->transcript.rbegin(); it != s->transcript.rend(); ++it) {
        if ((*it)->kind == sync::EntryKind::Notice) {
            return (*it)->text;
        }
    }
    return std::string();
}

void hubActions() {
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    // Before accounts, an account action is refused and counted.
    std::string why;
    CHECK(!hub.apply(act(1, sync::ActionKind::AddAccount, "", "work", "claude"), &why, "w"));
    CHECK(has({why}, "holds no accounts"));
    CHECK(store.applied("w") == 1);
    CHECK(hub.useAccounts(Registry{}, &keeper).empty());
    std::uint64_t n = 2;
    CHECK(hub.apply(act(n++, sync::ActionKind::AddAccount, "", "work", "claude"), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::AddAccount, "", "cx", "codex"), &why, "w"));
    CHECK(!hub.apply(act(n++, sync::ActionKind::AddAccount, "", "work", "claude"), &why, "w"));
    CHECK(has({why}, "already set up"));
    CHECK(!hub.apply(act(n++, sync::ActionKind::AddAccount, "", "bad name", "claude"), &why, "w"));
    CHECK(!hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "work"), &why, "w"));
    CHECK(has({why}, "no friend ada"));
    CHECK(hub.apply(act(n++, sync::ActionKind::AddFriend, "ada", ""), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "work"), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "cx"), &why, "w"));
    CHECK(!hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "nope"), &why, "w"));
    CHECK(!hub.apply(act(n++, sync::ActionKind::SetDefault, "ada", "nope"), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::SetDefault, "ada", "work"), &why, "w"));
    CHECK(store.applied("w") == n - 1);
    CHECK(hub.accounts()->friendNamed("ada")->accounts == (std::vector<std::string>{"work", "cx"}));
    CHECK(keeper.kept.back() == *hub.accounts());
    CHECK(has(keeper.messages, "asked by the window w"));
    // A keeper that refuses (hand edits pending) changes nothing.
    keeper.refuse = "hand edits pending";
    const Registry before = *hub.accounts();
    CHECK(!hub.apply(act(n++, sync::ActionKind::UnlinkAccount, "ada", "cx"), &why, "w"));
    CHECK(has({why}, "hand edits pending") && *hub.accounts() == before && store.applied("w") == n - 1);
    keeper.refuse.clear();

    // A create takes the friend's default, and runs its provider's harness.
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 0, "ada", "", ""}, &why, "w"));
    CHECK(store.find(1)->info.account == "work" && store.find(1)->info.harness == "claude");
    // A set to an account not linked is refused with a notice; to a linked one of another provider, its harness.
    CHECK(!hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "home", 1), &why, "w"));
    CHECK(has({lastNotice(store, 1)}, "home is not one of ada's accounts (no account home is set up"));
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "cx", 1), &why, "w"));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->info.harness == "codex");
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "", 1), &why, "w"));
    CHECK(store.find(1)->info.account.empty() && store.find(1)->info.harness == "codex");
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "work", 1), &why, "w"));
    // A create copying a session takes its account when linked to the new session's friend, else the default.
    CHECK(hub.apply(act(n++, sync::ActionKind::AddFriend, "bo", ""), &why, "w"));
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 1, "bo", "", ""}, &why, "w"));
    CHECK(store.find(2)->info.account.empty() && store.find(2)->info.harness == "claude");
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 1, "ada", "", ""}, &why, "w"));
    CHECK(store.find(3)->info.account == "work");

    // Unlinking moves the friend's sessions on it, each told; with nothing signed in, to none.
    CHECK(hub.apply(act(n++, sync::ActionKind::UnlinkAccount, "ada", "work"), &why, "w"));
    CHECK(store.find(1)->info.account.empty() && store.find(3)->info.account.empty());
    CHECK(has({lastNotice(store, 1)}, "The account work is no longer one of ada's accounts"));
    CHECK(hub.accounts()->friendNamed("ada")->preferred.empty());
    // With cx signed in, a move lands on it.
    CHECK(hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "work"), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "work", 1), &why, "w"));
    hub.reportSignedIn("cx", true);
    CHECK(hub.apply(act(n++, sync::ActionKind::RemoveAccount, "", "work"), &why, "w"));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->info.harness == "codex");
    CHECK(hub.accounts()->account("work") == nullptr);
    CHECK(hub.accounts()->friendNamed("ada")->accounts == (std::vector<std::string>{"cx"}));
    // A sign-in for an account not set up is ignored; an account removed and set up again needs sign-in again.
    hub.reportSignedIn("work", true);
    CHECK(hub.signedIn().count("work") == 0);
    CHECK(hub.apply(act(n++, sync::ActionKind::RemoveAccount, "", "cx"), &why, "w"));
    CHECK(store.find(1)->info.account.empty());
    CHECK(hub.apply(act(n++, sync::ActionKind::AddAccount, "", "cx", "codex"), &why, "w"));
    CHECK(hub.signedIn().count("cx") == 0);
}

void hubReloadAndRotate() {
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    std::string why;
    // A session from before accounts, on an account name the registry does not hold: settled at the start.
    CHECK(hub.apply(sync::Action{1, sync::ActionKind::Ensure, 0, "ada", "", ""}, &why, "w"));
    CHECK(hub.apply(act(2, sync::ActionKind::SetAccount, "", "", "old", 1), &why, "w"));
    std::vector<std::string> report = hub.useAccounts(sample(), &keeper);
    CHECK(has(report, "session 1 (ada): account old -> none"));
    CHECK(store.find(1)->info.account.empty());
    CHECK(hub.apply(act(3, sync::ActionKind::SetAccount, "", "", "work", 1), &why, "w"));

    // Rotation: the next signed-in link after the current one; none signed in, a notice and no move.
    CHECK(!hub.rotate(1, &why));
    CHECK(store.find(1)->info.account == "work");
    CHECK(has({lastNotice(store, 1)}, "has no other signed-in account"));
    hub.reportSignedIn("cx", true);
    hub.reportSignedIn("home", true);
    CHECK(hub.rotate(1, &why));
    CHECK(store.find(1)->info.account == "home");
    CHECK(hub.rotate(1, &why));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->info.harness == "codex");
    CHECK(has({lastNotice(store, 1)}, "reached its usage limit, so this session now runs on the account cx"));
    hub.reportSignedIn("home", false);
    CHECK(!hub.rotate(1, &why));
    CHECK(!hub.rotate(9, &why) && has({why}, "no session 9"));

    // A reload of an inconsistent edit is refused whole.
    Registry bad = *hub.accounts();
    bad.friends[0].preferred = "nope";
    CHECK(!hub.applyAccounts(bad, "reload", &report));
    CHECK(has(report, "\"default\" is \"nope\"") && hub.accounts()->friendNamed("ada")->preferred == "work");
    // A reload that changes cx's provider: the session takes the new harness, told.
    Registry edit = *hub.accounts();
    edit.accounts[2].provider = "claude";
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "account cx's provider codex -> claude"));
    CHECK(store.find(1)->info.harness == "claude" && has({lastNotice(store, 1)}, "provider is now claude"));
    // A reload that drops the friend: its session moves to none, told.
    edit.friends.erase(edit.friends.begin());
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "friend ada removed") && has(report, "session 1 (ada): account cx -> none"));
    CHECK(store.find(1)->info.account.empty());
    // The keeper was given each registry applied.
    CHECK(keeper.kept.size() == 2 && keeper.kept.back() == edit);
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"layout", layout},
    {"refusals", refusals},
    {"versions", versions},
    {"consistency", consistency},
    {"picks", picks},
    {"hub_actions", hubActions},
    {"hub_reload_rotate", hubReloadAndRotate},
};

} // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (const Group& g : kGroups) {
        if (argc > 1 && std::strcmp(argv[1], g.name) != 0) {
            continue;
        }
        g.run();
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "accounts_test: no group %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "accounts_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("accounts_test: ok\n");
    return 0;
}
