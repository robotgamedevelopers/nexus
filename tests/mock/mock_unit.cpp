// Unit tests of what the mock writes (tests/mock/mock.h): the names and presets, each AI's script (every message it
// emits decodes as a driver message, every streamed string reads back as written, every seeded prompt has its block),
// the drivers file as the server reads it, the accounts and AIs as the server reads them (no AI has an account but the
// sign-in preset's), the server's config, and the client command. Pure: no files, time, sockets or processes.

#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "driver/protocol.h"
#include "mock.h"
#include "server/driver_table.h"
#include "server/home.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

void presets() {
    std::vector<std::string> ais;
    std::string why;
    for (std::size_t n : {0u, 1u, 5u, 12u, 30u}) {
        CHECK(mock::aisFor(n, &ais, &why));
        CHECK(ais.size() == n);
        CHECK(std::set<std::string>(ais.begin(), ais.end()).size() == n);
        for (const std::string& name : ais)
            CHECK(accounts::isName(name) && name != mock::kSignInName);
    }
    CHECK(mock::aisFor(5, &ais, &why));
    CHECK((ais == std::vector<std::string>{"Ada", "Grace", "Linus", "Margaret", "Alan"}));
    CHECK(mock::aisFor(32, &ais, &why) && ais[30] == "mock-31" && ais[31] == "mock-32");
    CHECK(mock::aisFor(mock::kMaxAis, &ais, &why));
    CHECK(!mock::aisFor(mock::kMaxAis + 1, &ais, &why) && why.find("at most") != std::string::npos);

    CHECK(mock::aisFrom("Ann,Bea", &ais, &why) && (ais == std::vector<std::string>{"Ann", "Bea"}));
    CHECK(!mock::aisFrom("Ann,Ann", &ais, &why) && why.find("twice") != std::string::npos);
    CHECK(!mock::aisFrom("Ann,,Bea", &ais, &why) && why.find("not an AI's name") != std::string::npos);
    CHECK(!mock::aisFrom("Ann Lee", &ais, &why));
    CHECK(!mock::aisFrom(std::string(mock::kSignInName), &ais, &why) && why.find("sign-in") != std::string::npos);
}

// The script's lines, as the fake driver reads them (blank and # lines skipped).
std::vector<std::string> lines(const std::string& script) {
    std::vector<std::string> out;
    std::istringstream in(script);
    for (std::string line; std::getline(in, line);)
        if (!line.empty() && line[0] != '#')
            out.push_back(line);
    return out;
}

// What a JSON string literal holds, read with the protocol's own decoder (a text message carrying it).
std::string unquote(const std::string& literal) {
    const driver::Decoded<driver::DriverMessage> d =
        driver::decodeDriverMessage("{\"kind\":\"text\",\"turn\":1,\"delta\":" + literal + "}");
    const driver::Text* t = d.message ? std::get_if<driver::Text>(&*d.message) : nullptr;
    return t != nullptr ? t->delta : std::string("<not a string>");
}

void scripts() {
    for (bool instant : {false, true}) {
        const std::string script = mock::scriptFor("Ada", instant);
        std::set<std::string> prompts;
        int sends = 0;
        int paced = 0;
        bool cycle = false;
        for (const std::string& line : lines(script)) {
            const std::string op = line.substr(0, line.find(' '));
            const std::string arg = line.find(' ') == std::string::npos ? "" : line.substr(line.find(' ') + 1);
            if (op == "emit") {
                std::string json = arg;
                if (json.find("\"turn\"") == std::string::npos && json.find("\"hello\"") == std::string::npos &&
                        json.find("\"session\"") == std::string::npos)
                    json.insert(1, "\"turn\":1,");
                const auto d = driver::decodeDriverMessage(json);
                if (!d.message)
                    std::fprintf(stderr, "  %s: %s\n", json.c_str(), d.error.c_str());
                CHECK(d.message.has_value());
            } else if (op == "stream" || op == "pause") {
                const std::string ms = arg.substr(0, arg.find(' '));
                CHECK(!ms.empty() && ms.find_first_not_of("0123456789") == std::string::npos);
                paced += ms != "0";
                if (op == "stream")
                    CHECK(unquote(arg.substr(arg.find(' ') + 1)) != "<not a string>");
            } else if (op == "prompt") {
                CHECK(prompts.insert(arg).second);
            } else if (op == "send") {
                ++sends;
            } else if (op == "option") {
                cycle = cycle || arg == "cycle";
            } else {
                CHECK(op == "await-stop" || op == "exit");
            }
        }
        CHECK(cycle && sends == 3);
        CHECK(instant ? paced == 0 : paced > 0);
        // Each seeded prompt has its block, and each seeded reply is in the script as the transcript shows it.
        for (const mock::SeedTurn& t : mock::seedFor("Ada", 0)) {
            CHECK(prompts.count(t.prompt) == 1);
            CHECK(script.find(mock::jsonQuote(t.reply)) != std::string::npos);
            CHECK(t.prompt.find('\n') == std::string::npos);
        }
        CHECK(prompts.size() == mock::seedFor("Ada", 0).size());
        CHECK(script.find(mock::jsonQuote(mock::firstReplyText())) != std::string::npos);
    }
    // The seed has each kind of turn the clients must draw, in both orders, the same turns in each, ending with an
    // ordinary one; the even order ends with the code, the odd with the stopped turn, the error and the cut.
    const std::vector<mock::SeedTurn> even = mock::seedFor("Grace", 0);
    const std::vector<mock::SeedTurn> odd = mock::seedFor("Grace", 1);
    for (const auto* seed : {&even, &odd}) {
        std::set<int> ends;
        std::set<std::string> prompts;
        for (const mock::SeedTurn& t : *seed) {
            ends.insert(int(t.ends));
            prompts.insert(t.prompt);
        }
        CHECK(ends.size() == 4 && prompts.size() == seed->size());
        CHECK(seed->front().prompt.find("Grace") != std::string::npos);
        CHECK(seed->back().ends == mock::SeedTurn::Ends::Ok);
    }
    CHECK(even.size() == odd.size() && mock::seedFor("Grace", 2).back().prompt == even.back().prompt);
    CHECK(even.back().reply.find("```cpp") != std::string::npos);
    CHECK(odd[odd.size() - 2].ends == mock::SeedTurn::Ends::Cut);
    CHECK(unquote(mock::jsonQuote("a \"b\"\\\n\t\x01 é")) == "a \"b\"\\\n\t\x01 é");
}

void drivers() {
    mock::Options o;
    o.ais = {"Ada", "Grace"};
    o.signIn = true;
    const server::DriverTable t = server::parseDriverTable(mock::driversFor(o, "/b/fake", "/m/scripts", "/m/work"), "/h");
    CHECK(t.errors.empty());
    for (const char* name : {"Ada", "Grace", "Ida"}) {
        const server::ResolvedDriver r = server::resolveDriver(t, name, "drivers");
        CHECK(r.problem.empty());
        CHECK(r.cwd == std::string("/m/work/") + name);
        CHECK((r.command == std::vector<std::string>{"/b/fake", "--script", std::string("/m/scripts/") + name + ".script"}));
    }
    CHECK(t.ais.at("Ida").harness == "claude");
    CHECK(t.ais.at("Ada").harness != t.ais.at("Grace").harness);
}

void registry() {
    mock::Options o;
    o.ais = {"Ada", "Grace", "Linus"};
    accounts::Registry r = mock::registryFor(o, "/m/work");
    CHECK(r.accounts.empty() && r.ais.size() == 3);
    for (const accounts::AI& f : r.ais) {
        CHECK(f.accounts.empty() && f.preferred.empty() && !f.limit);
        CHECK(f.instances.size() == 1 && f.instances[0].machine == mock::kMachine &&
            f.instances[0].dir == "/m/work/" + f.name);
    }
    CHECK(r.ais[0].name == "Ada" && r.ais[2].name == "Linus");
    // As the server reads them back.
    accounts::Registry back;
    CHECK(accounts::readAccounts(accounts::accountsText(r), "accounts.json", &back).ok);
    CHECK(accounts::readAIs(accounts::aisText(r), "ais.json", &back).ok);
    CHECK(back == r);

    o.signIn = true;
    r = mock::registryFor(o, "/m/work");
    CHECK(r.accounts.size() == 1 && r.accounts[0].name == mock::kSignInAccount && r.accounts[0].provider == "claude" &&
        r.accounts[0].email.empty());
    CHECK(r.ais.size() == 4 && r.ais.back().name == mock::kSignInName &&
        r.ais.back().accounts == std::vector<std::string>{mock::kSignInAccount});
    accounts::Registry signIn;
    CHECK(accounts::readAccounts(accounts::accountsText(r), "accounts.json", &signIn).ok);
    CHECK(accounts::readAIs(accounts::aisText(r), "ais.json", &signIn).ok);
    CHECK(signIn == r);
    o.ais.clear();
    o.signIn = false;
    CHECK(mock::registryFor(o, "/m/work") == accounts::Registry{});
}

void config() {
    mock::Options o;
    server::ServerSettings s;
    std::vector<std::string> warnings;
    std::string why;
    CHECK(server::readServerConfig(mock::serverConfigFor(o, "/m/bin/claude-silent"), "config", &s, &warnings, &why));
    CHECK(s.machine == mock::kMachine && s.claude.empty() && warnings.empty());
    o.signIn = true;
    CHECK(server::readServerConfig(mock::serverConfigFor(o, "/m/bin/claude-silent"), "config", &s, &warnings, &why));
    CHECK(s.claude == "/m/bin/claude-silent");

    std::string command;
    CHECK(mock::serverCommandFor("/b/nexus-server", "/m/home", "/m/server", &command, &why));
    CHECK(command == "/usr/bin/env HOME=/m/home /b/nexus-server new --dir /m/server");
    CHECK(!mock::serverCommandFor("/b/nexus server", "/m/home", "/m/server", &command, &why));
    CHECK(!mock::serverCommandFor("/b/nexus-server", "m/home", "/m/server", &command, &why));
}

} // namespace

int main() {
    presets();
    scripts();
    drivers();
    registry();
    config();
    if (failures != 0) {
        std::fprintf(stderr, "mock_unit: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mock_unit: ok\n");
    return 0;
}
