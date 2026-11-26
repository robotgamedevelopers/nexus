// Unit tests of the accounts and AIs (src/accounts/registry.h) and of the hub's decisions about them
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
    r.ais = {{"ada", {"work", "home", "cx"}, "work"}, {"bo", {"home"}, ""}};
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
    const std::string f = accounts::aisText(r);
    CHECK(a ==
        "{\n  \"version\": 2,\n  \"accounts\": [\n    {\n      \"name\": \"work\",\n      \"provider\": \"claude\",\n"
        "      \"billing\": \"subscription\"\n    },\n    {\n      \"name\": \"home\",\n      \"provider\": \"claude\",\n"
        "      \"billing\": \"subscription\"\n    },\n    {\n      \"name\": \"cx\",\n      \"provider\": \"codex\",\n"
        "      \"billing\": \"subscription\"\n    }\n  ]\n}\n");
    CHECK(f.find("\"default\": null") != std::string::npos);
    CHECK(f.find("    {\n      \"name\": \"ada\",\n      \"limit\": true,\n      \"accounts\": [") != std::string::npos);
    CHECK(f.back() == '\n');
    Registry back;
    CHECK(accounts::readAccounts(a, "accounts.json", &back).ok);
    CHECK(accounts::readAIs(f, "ais.json", &back).ok);
    CHECK(back == r);
    // Writing what was read gives the same text: a save never reshuffles the lists.
    CHECK(accounts::accountsText(back) == a);
    CHECK(accounts::aisText(back) == f);
    Registry labeled = r;
    auto& account = labeled.accounts.front();
    account.label = "Work";
    account.email = "dev+work@example.test";
    account.signInOnUse = true;
    const std::string withOptions = accounts::accountsText(labeled);
    CHECK(withOptions.find("\"version\": 3") != std::string::npos);
    CHECK(accounts::readAccounts(withOptions, "accounts.json", &back).ok && back == labeled);
    CHECK(back.accounts[1].label.empty() && back.accounts[1].email.empty() && !back.accounts[1].signInOnUse);
    CHECK(accounts::accountsText(back) == withOptions);
    CHECK(has(accounts::changes(r, back), "display label changed"));
    Registry none;
    CHECK(accounts::accountsText(none) == "{\n  \"version\": 2,\n  \"accounts\": []\n}\n");
    CHECK(accounts::readAccounts(accounts::accountsText(none), "a", &back).ok && back.accounts.empty());
    // A string with a quote and a backslash survives.
    Registry odd;
    odd.ais = {{"x", {}, ""}};
    odd.accounts = {{"a", "claude"}};
    CHECK(accounts::readAIs(accounts::aisText(odd), "f", &back).ok && back.ais == odd.ais);
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
    rd = accounts::readAccounts(R"({"version":2,"accounts":[{"name":"w","provider":"codex","label":"x\ny"}]})", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "label must be one line"));
    rd = accounts::readAccounts(R"({"version":3,"accounts":[{"name":"w","provider":"codex","sign_in_on_use":"yes"}]})",
                                "a", &r);
    CHECK(!rd.ok && has(rd.errors, "not true or false"));
    rd = accounts::readAccounts("{\"version\": 1, \"accounts\": {}}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "\"accounts\" is an object, not a list"));
    rd = accounts::readAccounts("[]", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "the file is a list, not an object"));
    rd = accounts::readAccounts("{\"accounts\": []}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "has no \"version\""));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"accounts\": \"work\"}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "\"accounts\" is a string, not a list"));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"accounts\": [1]}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "holds a number, not an account's name"));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"default\": 3}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "\"default\" is a number"));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"name\": \"b\"}]}", "f", &r);
    CHECK(!rd.ok && has(rd.errors, "has the field \"name\" twice"));
    // Every error is found, not only the first.
    rd = accounts::readAccounts("{\"version\": 1, \"accounts\": [{\"name\": 1, \"provider\": \"claude\"}, "
                                "{\"name\": \"b\", \"provider\": 2}]}", "a", &r);
    CHECK(rd.errors.size() == 2);
    // An empty or missing file is none.
    CHECK(accounts::readAIs("", "f", &r).ok && r.ais.empty());
    CHECK(accounts::readAIs(" \n", "f", &r).ok);
}

void versions() {
    Registry r;
    // The version 1 fixture, written by hand, reads, and is upgraded: version 2 is the newest. Its accounts are a
    // subscription's, as every account was then.
    accounts::Read rd = accounts::readAccounts(kAccountsV1, "a", &r);
    CHECK(rd.ok && rd.version == 1 && rd.upgraded);
    CHECK(r.accounts.size() == 2 && r.accounts[0].name == "work" && r.accounts[1].provider == "codex");
    CHECK(r.accounts[0].billing == accounts::Billing::Subscription);
    // Written back in the one layout, in the person's order, in version 2.
    CHECK(accounts::accountsText(r).find("\"name\": \"work\"") < accounts::accountsText(r).find("\"name\": \"cx\""));
    CHECK(accounts::accountsText(r).find("\"version\": 2,") != std::string::npos);
    rd = accounts::readAccounts("{\"version\": 4, \"accounts\": []}", "/h/accounts.json", &r);
    CHECK(!rd.ok && has(rd.errors, "/h/accounts.json:1: \"version\" is 4, newer than this nexus-server reads (3)"));
    CHECK(has(rd.errors, "the file was left as it is"));
    rd = accounts::readAccounts("{\"version\": 1.5, \"accounts\": []}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "not a whole number"));
    rd = accounts::readAccounts("{\"version\": 0, \"accounts\": []}", "a", &r);
    CHECK(!rd.ok);
    // A version past any integer: refused with words, never cast (1e300 overflows a long long).
    rd = accounts::readAccounts("{\"version\": 1e300, \"accounts\": []}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "\"version\" is a number past any version, newer than this nexus-server reads"));
    rd = accounts::readAccounts("{\"version\": 2e9, \"accounts\": []}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "past any version"));
}

void consistency() {
    CHECK(accounts::problems(sample()).empty());
    Registry r = sample();
    r.ais[0].accounts.push_back("gone");
    CHECK(has(accounts::problems(r), "links to the account \"gone\", which accounts.json does not set up"));
    r = sample();
    r.ais[1].preferred = "work";
    CHECK(has(accounts::problems(r), "\"default\" is \"work\", which is not in its \"accounts\""));
    // A name or provider starting with '-' would read as an option where it reaches an argv (--account <name>).
    CHECK(!accounts::isName("-x") && !accounts::isName("--account") && accounts::isName("a-") && accounts::isName("_a"));
    CHECK(!accounts::isProvider("-claude") && accounts::isProvider("claude-2"));
    r = sample();
    r.accounts.push_back({"-rf", "claude"});
    CHECK(has(accounts::problems(r), "the account \"-rf\" is not a name"));
    r = sample();
    r.accounts.push_back({"work", "claude"});
    CHECK(has(accounts::problems(r), "the account \"work\" is there twice"));
    r = sample();
    r.ais[0].accounts.push_back("work");
    CHECK(has(accounts::problems(r), "links to the account \"work\" twice"));
    r = sample();
    r.ais.push_back({"bo", {}, ""});
    CHECK(has(accounts::problems(r), "the AI \"bo\" is there twice"));
}

// An AI's instances: the AI on a machine, in a directory. Read, written in the one layout, checked, and told
// apart in a reload's report.
void instances() {
    Registry r = sample();
    r.ais[0].instances = {{"studio", "studio", "/Volumes/nova/ai/rowan"}, {"rocketnet", "studio", "/work/rocketnet"},
                              {"laptop", "macbook.local", "/Users/me"}};
    CHECK(accounts::problems(r).empty());
    const std::string f = accounts::aisText(r);
    CHECK(f.find("      \"instances\": [\n        {\n          \"name\": \"studio\",\n          \"machine\": \"studio\",\n"
                 "          \"dir\": \"/Volumes/nova/ai/rowan\"\n        },") != std::string::npos);
    CHECK(f.find("\"default\": null,\n      \"instances\": []\n    }") != std::string::npos);   // bo has none
    Registry back;
    CHECK(accounts::readAIs(f, "ais.json", &back).ok && back.ais == r.ais);
    CHECK(accounts::aisText(back) == f);
    CHECK(r.ais[0].instance("rocketnet") != nullptr && r.ais[0].instance("nope") == nullptr);
    // A file without "instances" (as before them) reads as none.
    CHECK(accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\"}]}", "f", &back).ok &&
          back.ais[0].instances.empty());

    // Refusals, with the file's line.
    accounts::Read rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"instances\": "
        "{}}]}", "f", &back);
    CHECK(!rd.ok && has(rd.errors, "\"instances\" is an object, not a list"));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"instances\": [\"studio\"]}]}",
                               "f", &back);
    CHECK(!rd.ok && has(rd.errors, "holds a string, not an instance"));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"instances\": [\n"
        "{\"name\": \"s\", \"machine\": \"studio\", \"dir\": \"relative/path\"}]}]}", "/h/ais.json", &back);
    CHECK(!rd.ok && has(rd.errors, "/h/ais.json:2: the AI \"ada\"'s instance \"s\"'s dir \"relative/path\" is "
                        "not an absolute path"));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"instances\": ["
        "{\"name\": \"s\", \"machine\": \"me@studio:22\", \"dir\": \"/w\"}]}]}", "f", &back);
    CHECK(!rd.ok && has(rd.errors, "machine \"me@studio:22\" is not a host name"));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"instances\": ["
        "{\"name\": \"s\", \"machine\": \"studio\"}]}]}", "f", &back);
    CHECK(!rd.ok && has(rd.errors, "has no \"dir\""));
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\", \"instances\": ["
        "{\"name\": \"s\", \"machine\": \"studio\", \"dir\": \"/w\", \"host\": \"x\"}]}]}", "f", &back);
    CHECK(!rd.ok && has(rd.errors, "has a field \"host\""));
    CHECK(!accounts::isMachine("-studio") && !accounts::isMachine("a b") && accounts::isMachine("studio.local"));
    CHECK(!accounts::isDir("") && !accounts::isDir("~/x") && !accounts::isDir("/a\nb") && accounts::isDir("/"));

    // Consistency: each named once within its AI; the same name in two AIs is fine.
    Registry twice = r;
    twice.ais[0].instances.push_back({"studio", "studio", "/elsewhere"});
    CHECK(has(accounts::problems(twice), "the AI \"ada\"'s instance \"studio\" is there twice"));
    twice = r;
    twice.ais[1].instances = {{"studio", "studio", "/x"}};
    CHECK(accounts::problems(twice).empty());
    twice.ais[1].instances[0].machine = "-x";
    CHECK(has(accounts::problems(twice), "has the machine \"-x\", which is not a host name"));

    // A reload's report.
    Registry moved = r;
    moved.ais[0].instances[1].dir = "/work/rocketnet-2";
    moved.ais[0].instances.pop_back();
    moved.ais[0].instances.push_back({"mini", "mini-m5", "/w"});
    const std::vector<std::string> lines = accounts::changes(r, moved);
    CHECK(has(lines, "AI ada's instance rocketnet moved: studio /work/rocketnet -> studio /work/rocketnet-2"));
    CHECK(has(lines, "AI ada's instance laptop removed"));
    CHECK(has(lines, "AI ada's instance mini set up, on mini-m5 in /w"));
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
    CHECK(has(c, "account work set up (claude)") && has(c, "AI ada set up, accounts [work, home, cx], default work"));
    Registry less = r;
    less.ais[0].accounts.pop_back();
    less.accounts.pop_back();
    CHECK(has(accounts::changes(r, less), "account cx removed"));
    CHECK(has(accounts::changes(r, less), "AI ada's accounts [work, home, cx] -> [work, home]"));
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

sync::Action act(std::uint64_t num, sync::ActionKind kind, std::string aiName, std::string account,
                 std::string text = std::string(), sync::SessionId session = 0) {
    return sync::Action{num, kind, session, std::move(aiName), std::move(text), std::move(account)};
}

std::string lastNotice(const sync::SessionStore& store, sync::SessionId id) {
    const sync::StoredSession* s = store.find(id);
    for (auto it = s->transcript.rbegin(); it != s->transcript.rend(); ++it) {
        if ((*it)->kind == EntryKind::Notice) {
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
    CHECK(has({why}, "no AI ada"));
    CHECK(hub.apply(act(n++, sync::ActionKind::AddAI, "ada", ""), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "work"), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "cx"), &why, "w"));
    CHECK(!hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "nope"), &why, "w"));
    CHECK(!hub.apply(act(n++, sync::ActionKind::SetDefault, "ada", "nope"), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::SetDefault, "ada", "work"), &why, "w"));
    CHECK(store.applied("w") == n - 1);
    CHECK(hub.accounts()->aiNamed("ada")->accounts == (std::vector<std::string>{"work", "cx"}));
    CHECK(keeper.kept.back() == *hub.accounts());
    CHECK(has(keeper.messages, "asked by the window w"));
    // A keeper that refuses (hand edits pending) changes nothing.
    keeper.refuse = "hand edits pending";
    const Registry before = *hub.accounts();
    CHECK(!hub.apply(act(n++, sync::ActionKind::UnlinkAccount, "ada", "cx"), &why, "w"));
    CHECK(has({why}, "hand edits pending") && *hub.accounts() == before && store.applied("w") == n - 1);
    keeper.refuse.clear();

    // A create takes the AI's default, and runs its provider's harness.
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 0, "ada", "", ""}, &why, "w"));
    CHECK(store.find(1)->info.account == "work" && store.find(1)->info.autoAccount && store.find(1)->info.harness == "claude");
    // Repeated key presses resolve against the server's latest account, in link order, wrapping both ways.
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "+next", 1), &why, "w"));
    CHECK(store.find(1)->info.account == "cx");
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "+next", 1), &why, "w"));
    CHECK(store.find(1)->info.account == "work");
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "+previous", 1), &why, "w"));
    CHECK(store.find(1)->info.account == "cx");
    CHECK(!hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "+unknown", 1), &why, "w"));
    CHECK(store.find(1)->info.account == "cx");
    // A set to an account not linked is refused with a notice; to a linked one of another provider, its harness.
    CHECK(!hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "home", 1), &why, "w"));
    CHECK(has({lastNotice(store, 1)}, "home is not one of ada's accounts (no account home is set up"));
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "cx", 1), &why, "w"));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->info.harness == "codex");
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "", 1), &why, "w"));
    CHECK(store.find(1)->info.account.empty() && store.find(1)->info.harness == "codex");
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "work", 1), &why, "w"));
    // A create copying a session takes its account when linked to the new session's AI, else the default.
    CHECK(hub.apply(act(n++, sync::ActionKind::AddAI, "bo", ""), &why, "w"));
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 1, "bo", "", ""}, &why, "w"));
    CHECK(store.find(2)->info.account.empty() && store.find(2)->info.harness == "claude");
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 1, "ada", "", ""}, &why, "w"));
    CHECK(store.find(3)->info.account == "work");

    // Unlinking moves the AI's sessions on it, each told; with nothing signed in, to none.
    CHECK(hub.apply(act(n++, sync::ActionKind::UnlinkAccount, "ada", "work"), &why, "w"));
    CHECK(store.find(1)->info.account.empty() && store.find(3)->info.account.empty());
    CHECK(has({lastNotice(store, 1)}, "The account work is no longer one of ada's accounts"));
    CHECK(hub.accounts()->aiNamed("ada")->preferred.empty());
    // With cx signed in, a move lands on it.
    CHECK(hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "work"), &why, "w"));
    CHECK(hub.apply(act(n++, sync::ActionKind::SetAccount, "", "", "work", 1), &why, "w"));
    hub.reportSignedIn("cx", true);
    CHECK(hub.apply(act(n++, sync::ActionKind::RemoveAccount, "", "work"), &why, "w"));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->info.harness == "codex");
    CHECK(hub.accounts()->account("work") == nullptr);
    CHECK(hub.accounts()->aiNamed("ada")->accounts == (std::vector<std::string>{"cx"}));
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
    Registry registry = sample();
    registry.accounts[1].email = "subscriber@example.test";
    registry.accounts[1].label = "weekend";
    std::vector<std::string> report = hub.useAccounts(registry, &keeper);
    CHECK(has(report, "session 1 (ada): account old -> none"));
    CHECK(store.find(1)->info.account.empty());
    CHECK(hub.apply(act(3, sync::ActionKind::SetAccount, "", "", "work", 1), &why, "w"));

    CHECK(!store.find(1)->info.autoAccount && !hub.rotate(1, &why) && has({why}, "pinned"));
    CHECK(hub.apply(act(4, sync::ActionKind::SetAccount, "", "", sync::kAutoAccountTarget, 1), &why, "w"));
    CHECK(store.find(1)->info.autoAccount);

    // Rotation: the next signed-in link after the current one; none signed in, a notice and no move.
    CHECK(!hub.rotate(1, &why));
    CHECK(store.find(1)->info.account == "work");
    CHECK(has({lastNotice(store, 1)}, "has no other signed-in account"));
    hub.probeSignInWith([](const accounts::Account& a) { return a.email == "subscriber@example.test"; });
    hub.reportSignedIn("cx", true);
    hub.reportSignedIn("home", true);
    CHECK(hub.rotate(1, &why));
    CHECK(store.find(1)->info.account == "home");
    const auto& switched = *store.find(1)->transcript.back();
    CHECK(switched.text == "Switched to subscriber@example.test (weekend)");
    CHECK(switched.accountEmailEnd == std::string("Switched to subscriber@example.test").size());
    CHECK(hub.rotate(1, &why));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->info.harness == "codex");
    CHECK(lastNotice(store, 1) == "Switched to cx");
    bool exhausted = false;
    CHECK(!hub.rotate(1, &why, {"work", "home"}, &exhausted) && exhausted);
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->info.autoAccount);
    hub.reportSignedIn("home", false);
    CHECK(!hub.rotate(1, &why));
    CHECK(!hub.rotate(9, &why) && has({why}, "no session 9"));

    // A reload of an inconsistent edit is refused whole.
    Registry bad = *hub.accounts();
    bad.ais[0].preferred = "nope";
    CHECK(!hub.applyAccounts(bad, "reload", &report));
    CHECK(has(report, "\"default\" is \"nope\"") && hub.accounts()->aiNamed("ada")->preferred == "work");
    // A reload that changes cx's provider: the session takes the new harness, told.
    Registry edit = *hub.accounts();
    edit.accounts[2].provider = "claude";
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "account cx's provider codex -> claude"));
    CHECK(store.find(1)->info.harness == "claude" && has({lastNotice(store, 1)}, "provider is now claude"));
    // A reload that drops the AI: its session is kept as it is (its account too), told once.
    const Registry withAda = edit;
    const std::size_t entries = store.find(1)->transcript.size();
    edit.ais.erase(edit.ais.begin());
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "AI ada removed") && has(report, "session 1 (ada): its AI was removed"));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->transcript.size() == entries + 1);
    CHECK(has({lastNotice(store, 1)}, "ada was taken out of ais.json"));
    CHECK(has({lastNotice(store, 1)}, "its queued prompts are kept"));
    // A reload while it is out changes nothing of it; one that puts it back settles it: cx is still linked, so it stays.
    CHECK(hub.applyAccounts(edit, "reload again", &report));
    CHECK(store.find(1)->transcript.size() == entries + 1);
    CHECK(hub.applyAccounts(withAda, "reload", &report));
    CHECK(store.find(1)->info.account == "cx" && store.find(1)->transcript.size() == entries + 1);
    // The keeper was given each registry applied.
    CHECK(keeper.kept.size() == 4 && keeper.kept.back() == withAda);
}

// Auto routes only among linked subscriptions. A real registry account may itself be named "auto".
void hubAutoSubscriptions() {
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    Registry fleet = sample();
    fleet.accounts[0].billing = accounts::Billing::Api;
    fleet.accounts[1].name = "auto";
    fleet.ais[0].accounts = {"work", "auto", "cx"};
    fleet.ais[1].accounts.clear();
    hub.probeSignInWith([](const accounts::Account& a) { return a.name != "auto"; });
    CHECK(hub.useAccounts(fleet, &keeper).empty());
    std::uint64_t n = 0;
    auto make = [&](sync::SessionId from = 0) {
        return hub.apply(sync::Action{++n, sync::ActionKind::Create, from, "ada", "", ""}, nullptr, "w");
    };
    auto set = [&](const std::string& target) {
        return hub.apply(act(++n, sync::ActionKind::SetAccount, "", "", target, 1), nullptr, "w");
    };
    CHECK(make());
    CHECK(store.find(1)->info.autoAccount && store.find(1)->info.account == "cx");
    CHECK(make(1));
    CHECK(store.find(2)->info.autoAccount && store.find(2)->info.account == "cx");
    std::string why;
    CHECK(!hub.rotate(1, &why));
    CHECK(set("cx"));  // pin the same payer
    CHECK(!store.find(1)->info.autoAccount);
    CHECK(make(1));
    CHECK(!store.find(3)->info.autoAccount && store.find(3)->info.account == "cx");
    CHECK(set("auto") && !store.find(1)->info.autoAccount && store.find(1)->info.account == "auto");
    CHECK(set(sync::kAutoAccountTarget) && store.find(1)->info.autoAccount && store.find(1)->info.account == "cx");
    hub.reportSignedIn("auto", true);
    CHECK(hub.nextSubscription(1, {}, why) == "auto" && store.find(1)->info.account == "cx");
    CHECK(hub.rotate(1, &why) && store.find(1)->info.account == "auto");
    fleet.ais[0].accounts = {"work", "cx"};
    std::vector<std::string> report;
    CHECK(hub.applyAccounts(fleet, "reload", &report));
    CHECK(!store.find(1)->info.autoAccount && store.find(1)->info.account == "cx");
    CHECK(!hub.state().ais().front().canAutoAccount && !set(sync::kAutoAccountTarget));
}

// A memory store that fails from a chosen step on, as a disk that stops taking writes: that step and every later one
// refused, and failed() set.
class FailingStore : public sync::MemoryStore {
public:
    bool commit(sync::SessionId id, const std::vector<sync::SessionChange>& changes, const std::string& client,
                std::uint64_t num, std::string* why) override {
        if (failing || okLeft == 0) {
            broken = true;
        }
        if (okLeft > 0) {
            --okLeft;
        }
        if (broken) {
            *why = "the disk took no more";
            return false;
        }
        return sync::MemoryStore::commit(id, changes, client, num, why);
    }
    bool failed() const override { return broken; }
    bool failing = false;
    bool broken = false;
    int okLeft = -1;    // commits still taken before it fails (-1: no such count)
};

// The store fails after the keeper has committed: what was committed is reported applied, never "nothing applied",
// with the store's failure said, and the hub holds the committed registry (the server stops, and settles the sessions
// against it at its next start).
void hubStoreFails() {
    FailingStore store;
    sync::Hub hub(store);
    Keeper keeper;
    std::string why;
    CHECK(hub.apply(sync::Action{1, sync::ActionKind::Ensure, 0, "ada", "", ""}, &why, "w"));
    CHECK(hub.useAccounts(sample(), &keeper).empty());
    CHECK(hub.apply(act(2, sync::ActionKind::SetAccount, "", "", "work", 1), &why, "w"));
    // A reload that unlinks work: committed, then the store fails while the session moves.
    Registry edit = sample();
    edit.ais[0].accounts = {"home", "cx"};
    edit.ais[0].preferred = "home";
    store.failing = true;
    std::vector<std::string> report;
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "AI ada's accounts") && has(report, "the store failed while the sessions were moved"));
    CHECK(*hub.accounts() == edit && keeper.kept.back() == edit && hub.failed());
    // An account action: committed and taken, then the store fails as it is counted.
    FailingStore store2;
    sync::Hub hub2(store2);
    Keeper keeper2;
    CHECK(hub2.useAccounts(sample(), &keeper2).empty());
    store2.failing = true;
    CHECK(!hub2.apply(act(1, sync::ActionKind::AddAccount, "", "new", "claude"), &why, "w"));
    CHECK(has({why}, "the accounts were changed and committed") && has({why}, "the store failed"));
    CHECK(hub2.accounts()->account("new") != nullptr && keeper2.kept.size() == 1);
}

// After accounts.json was made from the drivers file: each session on no account whose AI has a default takes
// it, told; one already on an account, or whose AI has none, stays.
void hubAdoptDefaults() {
    sync::MemoryStore store;
    sync::Hub hub(store);
    std::string why;
    CHECK(hub.adoptDefaults().empty());     // no accounts held: nothing to adopt
    CHECK(hub.apply(sync::Action{1, sync::ActionKind::Ensure, 0, "ada", "", ""}, &why, "w"));
    CHECK(hub.apply(sync::Action{2, sync::ActionKind::Ensure, 0, "bo", "", ""}, &why, "w"));
    CHECK(hub.apply(sync::Action{3, sync::ActionKind::Ensure, 0, "cy", "", ""}, &why, "w"));
    Registry r;
    r.accounts = {{"work", "claude"}};
    r.ais = {{"ada", {"work"}, "work"}, {"bo", {"work"}, ""}};
    CHECK(hub.useAccounts(r, nullptr).empty());
    const std::vector<std::string> report = hub.adoptDefaults();
    CHECK((report == std::vector<std::string>{"session 1 (ada): account none -> work, as the drivers file ran it"}));
    CHECK(store.find(1)->info.account == "work" && store.find(1)->info.harness == "claude");
    CHECK(has({lastNotice(store, 1)}, "This session runs on the account work, which the drivers file gave ada"));
    CHECK(store.find(2)->info.account.empty() && store.find(3)->info.account.empty());
    CHECK(hub.adoptDefaults().empty());     // once on it, a session stays

    // A store that fails part way: the server stops, and its next start (a new hub on the same store) adopts the
    // rest; the session adopted before is left as it is.
    FailingStore failing;
    {
        sync::Hub first(failing);
        CHECK(first.apply(sync::Action{1, sync::ActionKind::Ensure, 0, "ada", "", ""}, &why, "w"));
        CHECK(first.apply(sync::Action{2, sync::ActionKind::Ensure, 0, "bo", "", ""}, &why, "w"));
        Registry both = r;
        both.ais[1].preferred = "work";
        CHECK(first.useAccounts(both, nullptr).empty());
        failing.okLeft = 1;
        CHECK(first.adoptDefaults().size() == 1 && first.failed());
        CHECK(failing.find(1)->info.account == "work" && failing.find(2)->info.account.empty());
        const std::size_t told1 = failing.find(1)->transcript.size();
        failing.broken = false;
        failing.okLeft = -1;
        sync::Hub next(failing);
        CHECK(next.useAccounts(both, nullptr).empty());
        CHECK((next.adoptDefaults() == std::vector<std::string>{
            "session 2 (bo): account none -> work, as the drivers file ran it"}));
        CHECK(failing.find(1)->transcript.size() == told1 && failing.find(2)->info.account == "work");
    }
}

// The sign-in probe: asked of each account as it first appears (at the start, every one), before the sessions are
// settled; a driver's report overrides it; an account already held is not asked again.
void hubProbe() {
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    std::vector<std::string> asked;
    hub.probeSignInWith([&](const accounts::Account& a) {
        asked.push_back(a.name);
        return a.name != "home";
    });
    std::string why;
    CHECK(hub.apply(sync::Action{1, sync::ActionKind::Ensure, 0, "ada", "", ""}, &why, "w"));
    CHECK(hub.apply(act(2, sync::ActionKind::SetAccount, "", "", "gone", 1), &why, "w"));
    // ada's session is on an account not held, so it lands on her default, work, which the probe found signed in.
    const std::vector<std::string> report = hub.useAccounts(sample(), &keeper);
    CHECK((asked == std::vector<std::string>{"work", "home", "cx"}));
    CHECK(hub.signedIn() == (std::set<std::string>{"work", "cx"}));
    CHECK(has(report, "session 1 (ada): account gone -> work"));
    hub.reportSignedIn("work", false);
    asked.clear();
    CHECK(hub.apply(act(3, sync::ActionKind::AddAccount, "", "new", "claude"), &why, "w"));
    CHECK((asked == std::vector<std::string>{"new"}));
    CHECK(hub.signedIn() == (std::set<std::string>{"cx", "new"}));
}

// The hub and instances: a create takes the instance of the session it copies when that is the same AI's, else
// the AI's first; a session whose instance goes moves to the first, told; an AI with none gives none.
void hubInstances() {
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    std::string why;
    Registry r = sample();
    r.ais[0].instances = {{"studio", "studio", "/a"}, {"rocketnet", "studio", "/b"}};
    CHECK(hub.useAccounts(r, &keeper).empty());
    CHECK(hub.apply(sync::Action{1, sync::ActionKind::Create, 0, "ada", "", ""}, &why, "w"));
    CHECK(store.find(1)->info.instance == "studio");
    CHECK(hub.apply(sync::Action{2, sync::ActionKind::Create, 0, "bo", "", ""}, &why, "w"));
    CHECK(store.find(2)->info.instance.empty());
    // Copying: put session 1 on rocketnet (an edit that drops studio moves it there), then copy it.
    Registry edit = r;
    edit.ais[0].instances.erase(edit.ais[0].instances.begin());
    std::vector<std::string> report;
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "AI ada's instance studio removed"));
    CHECK(has(report, "session 1 (ada): instance studio -> rocketnet"));
    CHECK(store.find(1)->info.instance == "rocketnet");
    CHECK(has({lastNotice(store, 1)}, "The instance studio is no longer one of ada's"));
    edit.ais[0].instances.insert(edit.ais[0].instances.begin(), {"studio", "studio", "/a"});
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(store.find(1)->info.instance == "rocketnet");     // its instance is still there: it stays
    CHECK(hub.apply(sync::Action{3, sync::ActionKind::Create, 1, "ada", "", ""}, &why, "w"));
    CHECK(store.find(3)->info.instance == "rocketnet");
    CHECK(hub.apply(sync::Action{4, sync::ActionKind::Create, 1, "bo", "", ""}, &why, "w"));
    CHECK(store.find(4)->info.instance.empty());            // another AI's: never copied
    // An instance moved in place (its directory edited, its name kept): every session on it is told, and stays on it.
    const std::size_t notices1 = store.find(1)->transcript.size();
    const std::size_t notices2 = store.find(2)->transcript.size();
    edit.ais[0].instances[1].dir = "/c";
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "session 1 (ada): instance rocketnet moved, on studio in /b -> on studio in /c"));
    CHECK(has(report, "session 3 (ada): instance rocketnet moved"));
    CHECK(store.find(1)->info.instance == "rocketnet" && store.find(1)->transcript.size() == notices1 + 1);
    CHECK(has({lastNotice(store, 1)}, "ada's instance rocketnet moved"));
    CHECK(has({lastNotice(store, 1)}, "is now on studio in /c"));
    CHECK(store.find(2)->transcript.size() == notices2);    // bo's: not told
    // An AI's first instance: its sessions on none take it, told (a session belongs to one of its AI's
    // instances while it has any).
    edit.ais[1].instances = {{"laptop", "laptop", "/home/bo"}};
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(store.find(2)->info.instance == "laptop" && store.find(4)->info.instance == "laptop");
    CHECK(has(report, "session 2 (bo): instance none -> laptop"));
    CHECK(has({lastNotice(store, 2)}, "bo's default instance, laptop (on laptop in /home/bo)"));
    // The same at a start: a session on none whose AI has instances takes the default.
    {
        sync::MemoryStore store2;
        sync::Hub hub2(store2);
        CHECK(hub2.apply(sync::Action{1, sync::ActionKind::Ensure, 0, "bo", "", ""}, &why, "w"));
        CHECK(store2.find(1)->info.instance.empty());
        Keeper keeper2;
        CHECK(has(hub2.useAccounts(edit, &keeper2), "session 1 (bo): instance none -> laptop"));
        CHECK(store2.find(1)->info.instance == "laptop");
    }
    // The AI dropped: its sessions keep their instance while it is out, told that it was taken out.
    const Registry withAda = edit;
    edit.ais.erase(edit.ais.begin());
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(store.find(1)->info.instance == "rocketnet" && store.find(3)->info.instance == "rocketnet");
    CHECK(has(report, "session 3 (ada): its AI was removed"));
    // Put back without rocketnet: settled as any other, moved to its first instance, told.
    Registry back = withAda;
    back.ais.front().instances.pop_back();
    CHECK(hub.applyAccounts(back, "reload", &report));
    CHECK(store.find(1)->info.instance == "studio" && has(report, "session 1 (ada): instance rocketnet -> studio"));
}

// The cap on AIs: an ais.json of kMaxAIs AIs reads; one more is refused whole, in one line with the
// count, the cap and what to do, in version 1 (before any upgrade) and in version 2 alike; a window's AddAI at the
// cap is refused and changes nothing.
void aisCap() {
    for (int version : {1, 2}) {
        std::string text = "{\"version\": " + std::to_string(version) + ", \"ais\": [";
        for (std::size_t i = 0; i <= accounts::kMaxAIs; ++i) {
            text += std::string(i == 0 ? "" : ", ") + "{\"name\": \"f" + std::to_string(i) + "\"}";
        }
        Registry r;
        accounts::Read rd = accounts::readAIs(text + "]}", "ais.json", &r);
        CHECK(!rd.ok && rd.errors.size() == 1 && r.ais.empty());
        CHECK(has(rd.errors, "holds 257 AIs, and nexus-server takes at most 256"));
    }
    auto file = [](std::size_t n) {
        std::string text = "{\"version\": 1, \"ais\": [";
        for (std::size_t i = 0; i < n; ++i) {
            text += std::string(i == 0 ? "" : ", ") + "{\"name\": \"f" + std::to_string(i) + "\"}";
        }
        return text + "]}";
    };
    Registry r;
    accounts::Read rd = accounts::readAIs(file(accounts::kMaxAIs), "ais.json", &r);
    CHECK(rd.ok && r.ais.size() == accounts::kMaxAIs);
    rd = accounts::readAIs(file(accounts::kMaxAIs + 1), "ais.json", &r);
    CHECK(!rd.ok && rd.errors.size() == 1 && r.ais.empty());
    CHECK(has(rd.errors, "ais.json:1: \"ais\" holds 257 AIs, and nexus-server takes at most 256; take "
                         "AIs out until it holds 256 or fewer; nothing was changed"));

    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    std::string why;
    CHECK(accounts::readAIs(file(accounts::kMaxAIs), "ais.json", &r).ok);
    CHECK(hub.useAccounts(r, &keeper).empty());
    CHECK(!hub.apply(act(1, sync::ActionKind::AddAI, "one-more", ""), &why, "w"));
    CHECK(has({why}, "ais.json holds 256 AIs, the most it may (256); take one out to add one-more"));
    CHECK(hub.accounts()->ais.size() == accounts::kMaxAIs && keeper.kept.empty());
}

// Repeating valid labels across AIs used to publish a roster that every window refused.
void pickerBudget() {
    Registry roster;
    std::vector<std::string> links;
    for (int i = 0; i < 100; ++i) {
        auto name = std::to_string(i);
        name.append(64 - name.size(), 'a');
        links.push_back(name);
        roster.accounts.push_back({name, "fake"});
    }
    for (std::size_t i = 0; i < accounts::kMaxAIs; ++i) {
        roster.ais.push_back({"ai" + std::to_string(i), links, ""});
    }
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    CHECK(hub.useAccounts(roster, &keeper).empty()); // all 25,600 choices fit without the labels
    sync::Sessions window;
    window.apply_string(hub.state().init_diff());
    CHECK(window.ais() == hub.state().ais());
    CHECK(window.ais().size() == accounts::kMaxAIs && window.ais().back().accounts.size() == 100);
    for (const auto& [label, email] : {std::pair{51, 0}, std::pair{25, 26}, std::pair{0, 254}}) {
        // At 51 added bytes the strings fit and framing pushes the roster over. At 254 bytes,
        // the emails alone exceed the limit before the server expands the repeated choices.
        for (auto& a : roster.accounts) {
            a.label.assign(label, 'L');
            a.email.assign(email, 'E');
        }
        std::vector<std::string> report;
        CHECK(!hub.applyAccounts(roster, "oversized edit", &report));
        CHECK(has(report, "linked account choices exceed the window roster budget"));
        CHECK(keeper.kept.empty() && hub.accounts()->accounts.front().label.empty() && hub.accounts()->accounts.front().email.empty());
        CHECK(window.ais() == hub.state().ais());
    }
}

// An AI's "limit" and an account's "billing" (docs/accounts.md): the defaults by version, the migration of a
// version 1 file, the refusals with their line, the always-limited rule for an API-billed account, the decision at a
// session's start (limitedFor), and the hub's reload of a limit, which tells the AI's sessions.
void limits() {
    Registry r;
    // Version 2: an AI with no "limit" is limited; one may say false. An account with no "billing" is a
    // subscription's.
    accounts::Read rd = accounts::readAIs("{\"version\": 2, \"ais\": [{\"name\": \"ada\"}, "
        "{\"name\": \"admin\", \"limit\": false}]}", "f", &r);
    CHECK(rd.ok && !rd.upgraded && r.ais.size() == 2 && r.ais[0].limit && !r.ais[1].limit);
    rd = accounts::readAccounts("{\"version\": 2, \"accounts\": [{\"name\": \"w\", \"provider\": \"claude\"}, "
        "{\"name\": \"fleet\", \"provider\": \"claude\", \"billing\": \"api\"}]}", "a", &r);
    CHECK(rd.ok && r.accounts[0].billing == accounts::Billing::Subscription &&
          r.accounts[1].billing == accounts::Billing::Api);
    // Version 1, from before the field: every AI reads as it ran then, unlimited, and is written explicitly from
    // then on (the upgrade, or the next applied change), so nothing changes under the person silently.
    rd = accounts::readAIs("{\"version\": 1, \"ais\": [{\"name\": \"ada\"}, {\"name\": \"bo\", \"limit\": "
        "true}]}", "f", &r);
    CHECK(rd.ok && rd.upgraded && !r.ais[0].limit && r.ais[1].limit);
    const std::string written = accounts::aisText(r);
    CHECK(written.find("\"version\": 2,") != std::string::npos);
    CHECK(written.find("\"name\": \"ada\",\n      \"limit\": false,") != std::string::npos);
    Registry back;
    CHECK(accounts::readAIs(written, "f", &back).ok && back.ais == r.ais);
    // Refusals, with the file's line and the fix.
    rd = accounts::readAIs("{\"version\": 2, \"ais\": [\n{\"name\": \"ada\", \"limit\": \"yes\"}]}",
                               "/h/ais.json", &r);
    CHECK(!rd.ok && has(rd.errors, "/h/ais.json:2: the AI \"ada\"'s \"limit\" is a string, not true or false"));
    CHECK(has(rd.errors, "write true or false, without quotes"));
    rd = accounts::readAccounts("{\"version\": 2, \"accounts\": [\n{\"name\": \"w\", \"provider\": \"claude\", "
        "\"billing\": \"API\"}]}", "/h/accounts.json", &r);
    CHECK(!rd.ok && has(rd.errors, "/h/accounts.json:2: the account \"w\"'s \"billing\" is \"API\", which is not one of"));
    rd = accounts::readAccounts("{\"version\": 2, \"accounts\": [{\"name\": \"w\", \"provider\": \"claude\", "
        "\"billing\": 1}]}", "a", &r);
    CHECK(!rd.ok && has(rd.errors, "\"billing\" is a number, not a string"));

    // The fixture: an API-billed account, as a fleet worker's would be.
    Registry fleet;
    fleet.accounts = {{"work", "claude"}, {"fleet", "claude", accounts::Billing::Api}};
    fleet.ais = {{"ada", {"work"}, "work"}, {"worker", {"fleet"}, "fleet"}};
    fleet.ais[0].limit = false;
    CHECK(accounts::problems(fleet).empty());
    CHECK(accounts::accountsText(fleet).find("\"name\": \"fleet\",\n      \"provider\": \"claude\",\n      "
                                             "\"billing\": \"api\"") != std::string::npos);
    // Setting limit false for an AI on an API-billed account is refused, with the reason.
    Registry bad = fleet;
    bad.ais[1].limit = false;
    CHECK(has(accounts::problems(bad), "the AI \"worker\" has \"limit\": false and links to the account \"fleet\", "
                                       "which accounts.json bills at API rate"));
    CHECK(has(accounts::problems(bad), "always limited"));
    bad = fleet;
    bad.ais[0].accounts.push_back("fleet");     // an unlimited AI linked to it: the same refusal
    CHECK(has(accounts::problems(bad), "the AI \"ada\" has \"limit\": false"));
    bad = fleet;
    bad.accounts[0].billing = accounts::Billing::Api;   // an account an unlimited AI runs on, made API-billed
    CHECK(has(accounts::problems(bad), "links to the account \"work\", which accounts.json bills at API rate"));

    // The decision at a session's start.
    CHECK(!accounts::limitedFor(&fleet, "ada", "work"));
    CHECK(!accounts::limitedFor(&fleet, "ada", ""));
    CHECK(accounts::limitedFor(&fleet, "worker", "fleet"));
    CHECK(accounts::limitedFor(&fleet, "nobody", "work"));      // an AI nexus does not hold: limited
    CHECK(accounts::limitedFor(nullptr, "ada", "work"));        // no accounts and AIs held: limited
    CHECK(accounts::limitedFor(&fleet, "ada", "gone"));        // an account the files do not hold: limited
    // Even where the files' rule were bypassed, an API-billed account's session is limited (the rule at the start).
    bad = fleet;
    bad.ais[0].accounts.push_back("fleet");
    CHECK(accounts::limitedFor(&bad, "ada", "fleet") && !accounts::limitedFor(&bad, "ada", "work"));
    CHECK(has(accounts::changes(fleet, bad), "AI ada's accounts [work] -> [work, fleet]"));
    Registry flipped = fleet;
    flipped.ais[0].limit = true;
    flipped.accounts[0].billing = accounts::Billing::Api;
    CHECK(has(accounts::changes(fleet, flipped), "AI ada's limit false -> true"));
    CHECK(has(accounts::changes(fleet, flipped), "account work's billing subscription -> api"));

    // The hub: an AI made by a window is limited; linking an API-billed account to an unlimited AI is
    // refused, changing nothing; a reload that sets limit false there is refused whole; a reload that changes an
    // AI's limit tells each of its sessions.
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    CHECK(hub.useAccounts(fleet, &keeper).empty());
    std::string why;
    std::uint64_t n = 1;
    CHECK(hub.apply(act(n++, sync::ActionKind::AddAI, "cy", ""), &why, "w"));
    CHECK(hub.accounts()->aiNamed("cy")->limit);
    CHECK(has(keeper.messages, "AI cy set up, accounts [], default none, instances [], limit true"));
    const Registry held = *hub.accounts();
    CHECK(!hub.apply(act(n++, sync::ActionKind::LinkAccount, "ada", "fleet"), &why, "w"));
    CHECK(has({why}, "fleet is billed at API rate") && has({why}, "ada has \"limit\": false"));
    CHECK(*hub.accounts() == held);
    CHECK(hub.apply(act(n++, sync::ActionKind::LinkAccount, "cy", "fleet"), &why, "w"));   // limited: fine
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 0, "ada", "", ""}, &why, "w"));
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 0, "worker", "", ""}, &why, "w"));
    std::vector<std::string> report;
    Registry edit = *hub.accounts();
    edit.aiNamed("worker")->limit = false;
    const Registry before = *hub.accounts();
    CHECK(!hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "the AI \"worker\" has \"limit\": false") && *hub.accounts() == before);
    edit = *hub.accounts();
    edit.aiNamed("ada")->limit = true;
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "AI ada's limit false -> true") && has(report, "session 1 (ada): limit false -> true"));
    CHECK(has({lastNotice(store, 1)}, "ada is now limited to its directory"));
    CHECK(lastNotice(store, 2).empty());   // worker's session: its AI's limit did not change
    CHECK(accounts::limitedFor(hub.accounts(), "ada", "work"));
}

// ais.json's "limit_keeps_out" (docs/accounts.md): absent is none; each entry an absolute directory other than the
// root, there once, refused with its line otherwise; written back only when it holds one, after "version"; a change is
// one report line, and a reload tells each limited session and no unlimited one.
void keepsOut() {
    Registry r;
    accounts::Read rd = accounts::readAIs("{\"version\": 2, \"ais\": []}", "f", &r);
    CHECK(rd.ok && r.keepsOut.empty() && !rd.upgraded);
    // A file from before the friend names became AI: its list under the old key reads, as an upgrade; "ais" beside it
    // leaves the old key unknown, refused.
    rd = accounts::readAIs("{\"version\": 2, \"friends\": [{\"name\": \"ada\"}]}", "f", &r);
    CHECK(rd.ok && rd.upgraded && r.ais.size() == 1 && r.ais[0].name == "ada");
    CHECK(accounts::aisText(r).find("\"ais\": [") != std::string::npos);
    rd = accounts::readAIs("{\"version\": 2, \"ais\": [], \"friends\": []}", "f", &r);
    CHECK(!rd.ok);
    CHECK(accounts::aisText(r).find("limit_keeps_out") == std::string::npos);
    rd = accounts::readAIs("{\"version\": 2, \"limit_keeps_out\": [\"/data\", \"/mnt/net\"], \"ais\": "
        "[{\"name\": \"ada\"}]}", "f", &r);
    CHECK(rd.ok && (r.keepsOut == std::vector<std::string>{"/data", "/mnt/net"}));
    const std::string written = accounts::aisText(r);
    CHECK(written.find("  \"version\": 2,\n  \"limit_keeps_out\": [\n    \"/data\",\n    \"/mnt/net\"\n  ],\n"
                       "  \"ais\": [") != std::string::npos);
    Registry back;
    CHECK(accounts::readAIs(written, "f", &back).ok && back == r);
    // Refusals, with the line and the fix.
    rd = accounts::readAIs("{\"version\": 2,\n\"limit_keeps_out\": \"/data\", \"ais\": []}", "/h/ais.json",
                               &r);
    CHECK(!rd.ok && has(rd.errors, "/h/ais.json:2: \"limit_keeps_out\" is a string, not a list"));
    rd = accounts::readAIs("{\"version\": 2, \"limit_keeps_out\": [\n\"data\",\n\"/\",\n1,\n\"/d\",\n\"/d\"], "
        "\"ais\": []}", "/h/ais.json", &r);
    CHECK(!rd.ok && has(rd.errors, "/h/ais.json:2: \"limit_keeps_out\" holds \"data\", which is not an absolute "
                                   "directory other than \"/\""));
    CHECK(has(rd.errors, "/h/ais.json:3: \"limit_keeps_out\" holds \"/\", which is not"));
    CHECK(has(rd.errors, "/h/ais.json:4: \"limit_keeps_out\" holds a number, not a directory"));
    CHECK(has(rd.errors, "/h/ais.json:6: \"limit_keeps_out\" holds \"/d\" twice"));
    // The consistency check, for a registry not read from a file (the window's edits).
    Registry bad;
    bad.keepsOut = {"/", "rel", "/d", "/d"};
    CHECK(has(accounts::problems(bad), "\"limit_keeps_out\" holds \"/\", which is not"));
    CHECK(has(accounts::problems(bad), "\"limit_keeps_out\" holds \"rel\", which is not"));
    CHECK(has(accounts::problems(bad), "\"limit_keeps_out\" holds \"/d\" twice"));
    Registry fine;
    fine.keepsOut = {"/data"};
    CHECK(accounts::problems(fine).empty());
    CHECK(has(accounts::changes(Registry{}, fine), "limit_keeps_out [] -> [/data]"));

    // The hub: a reload that changes the list tells each limited session, and no unlimited one.
    Registry fleet;
    fleet.accounts = {{"work", "claude"}};
    fleet.ais = {{"ada", {"work"}, "work"}, {"bo", {"work"}, "work"}};
    fleet.ais[1].limit = false;
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    CHECK(hub.useAccounts(fleet, &keeper).empty());
    std::string why;
    std::uint64_t n = 1;
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 0, "ada", "", ""}, &why, "w"));
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Create, 0, "bo", "", ""}, &why, "w"));
    Registry edit = *hub.accounts();
    edit.keepsOut = {"/data"};
    std::vector<std::string> report;
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(has(report, "limit_keeps_out [] -> [/data]"));
    CHECK(has({lastNotice(store, 1)}, "kept out of, on top of its own, are now /data"));
    CHECK(lastNotice(store, 2).empty());
    CHECK(hub.accounts()->keepsOut == std::vector<std::string>{"/data"});
}

// The state the windows receive carries the registry's AIs, in its order (sync/hub.h stateAIs): none known
// before the hub holds a registry; set when it takes one; changed by a reload that adds or drops an AI and by a
// window's AddAI; and kept across the state's rebuilds (a session made).
void hubAIs() {
    using Names = std::vector<sync::ServerAI>;
    sync::MemoryStore store;
    sync::Hub hub(store);
    Keeper keeper;
    CHECK(!hub.state().aisKnown() && hub.state().ais().empty());
    Registry edit = sample();
    edit.accounts[0].label = "Work";
    edit.accounts[0].email = "dev+work@example.test";
    edit.accounts.push_back({"unused", "fake", accounts::Billing::Subscription, "", false, "unlinked@example.test"});
    sync::ServerAI ada{"ada", {{"work", "Work", false, "dev+work@example.test"}, {"home"}, {"cx"}}};
    ada.canAutoAccount = true;
    const sync::ServerAI bo{"bo", {{"home"}}};
    CHECK(hub.useAccounts(edit, &keeper).empty());
    CHECK(hub.state().aisKnown() && (hub.state().ais() == Names{ada, bo}));
    sync::Sessions window;
    window.apply_string(hub.state().init_diff());
    CHECK((window.ais() == Names{ada, bo}));
    CHECK(hub.state().init_diff().find("unlinked@example.test") == std::string::npos);
    const auto before = hub.state();
    edit.accounts[0].email = "replacement@example.test";
    std::vector<std::string> report;
    CHECK(hub.applyAccounts(edit, "email edited", &report));
    window.apply_string(hub.state().diff_from(before));
    ada.accounts[0].email = edit.accounts[0].email;
    CHECK((window.ais() == Names{ada, bo}));
    // A session made rebuilds the state: the AIs stay.
    std::string why;
    std::uint64_t n = 1;
    CHECK(hub.apply(sync::Action{n++, sync::ActionKind::Ensure, 0, "ada", "", ""}, &why, "w"));
    CHECK(hub.state().sessions().size() == 1 && (hub.state().ais() == Names{ada, bo}));
    // A reload that drops bo and adds cy.
    edit.accounts[0].label = "dev@example.test";
    ada.accounts[0].label = edit.accounts[0].label;
    edit.ais.erase(edit.ais.begin() + 1);
    edit.ais.push_back(accounts::AI{"cy", {}, ""});
    CHECK(hub.applyAccounts(edit, "reload", &report));
    CHECK(hub.state().aisKnown() && (hub.state().ais() == Names{ada, {"cy"}}));
    // A window's AddAI.
    CHECK(hub.apply(act(n++, sync::ActionKind::AddAI, "di", ""), &why, "w"));
    CHECK((hub.state().ais() == Names{ada, {"cy"}, {"di"}}));
    // A registry with no AIs: known, and none.
    CHECK(hub.applyAccounts(Registry{sample().accounts, {}}, "reload", &report));
    CHECK(hub.state().aisKnown() && hub.state().ais().empty());
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
    {"hub_auto_subscriptions", hubAutoSubscriptions},
    {"hub_store_fails", hubStoreFails},
    {"hub_adopt_defaults", hubAdoptDefaults},
    {"hub_probe", hubProbe},
    {"instances", instances},
    {"hub_instances", hubInstances},
    {"limits", limits},
    {"keeps_out", keepsOut},
    {"hub_ais", hubAIs},
    {"ais_cap", [] { aisCap(); pickerBudget(); }},
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
