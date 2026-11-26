// The mock's pure part: see mock.h.

#include "mock.h"

#include <set>
#include <sstream>

namespace nexus::mock {

namespace {

// The pause before a reply's first word and between its words, in milliseconds, at a reading pace.
constexpr int kThinkMs = 700;
constexpr int kWordMs = 35;
constexpr int kToolMs = 450;

const char* const kLong =
    "This week is about making the server boring in the best sense. Monday goes to the store: the journal's compaction "
    "has a corner where a crash between the rename and the directory flush leaves two generations on disk, and the "
    "reader picks the older one, which is safe but loses the last batch. Tuesday and Wednesday go to the link, where a "
    "window that reconnects during a handover sometimes sends its unacknowledged actions to the old server first; the "
    "model already shows the trace, so the fix is mostly plumbing and a test that kills the server at each step. "
    "Thursday is for the drivers: the Claude driver's stop handling is fine, but the error it reports when the harness "
    "vanishes reads like a person's mistake, and it should say what happened and what to do instead. Friday is review "
    "and a point release, with nothing new started after lunch.";

const char* const kMarkdown =
    "## Release checklist\n\n"
    "Before tagging, every item below has to be true on `main` as it is, not on a branch.\n\n"
    "### Build\n\n"
    "1. A clean configure on each platform: macOS, Linux and Windows.\n"
    "2. Zero warnings, with the warnings the project sets.\n"
    "3. The unit tests and the functional tests green, with no test skipped.\n\n"
    "### Store and link\n\n"
    "- The store reads every older format it promises to read.\n"
    "- A window from the last release connects and works.\n"
    "  - Its kept drafts come back.\n"
    "  - Its actions are applied once each.\n\n"
    "### Words\n\n"
    "> The release notes say what changed for a person, in their words, not ours.\n\n"
    "**Then** cut the release with the repository's own tooling, never a bare tag.";

const char* const kCode =
    "Here is the main loop as it stands. The interesting part is the second branch, where a partial line is kept for the "
    "next read:\n\n"
    "```cpp\n"
    "// Reads lines from `fd` until it ends, handing each whole line to `onLine` and keeping a partial one for the next read.\n"
    "void Parser::run(int fd, const std::function<void(std::string_view line, std::size_t number)>& onLine) {\n"
    "    char buffer[4096];\n"
    "    for (;;) {\n"
    "        const ssize_t n = ::read(fd, buffer, sizeof buffer);\n"
    "        if (n == 0) {\n"
    "            if (!pending_.empty()) onLine(pending_, ++number_);   // the last line, with no newline after it\n"
    "            return;\n"
    "        }\n"
    "        if (n < 0) { if (errno == EINTR) continue; throw std::system_error(errno, std::generic_category(), \"read\"); }\n"
    "        std::string_view chunk(buffer, std::size_t(n));\n"
    "        for (std::size_t newline; (newline = chunk.find('\\n')) != std::string_view::npos; chunk.remove_prefix(newline + 1)) {\n"
    "            pending_.append(chunk.substr(0, newline));\n"
    "            onLine(pending_, ++number_);\n"
    "            pending_.clear();\n"
    "        }\n"
    "        pending_.append(chunk);\n"
    "    }\n"
    "}\n"
    "```\n\n"
    "The line limit is checked in `onLine`, so a line that never ends still grows `pending_` without bound; that is the "
    "bug we talked about yesterday.";

const char* const kToolsBefore = "I'll run the unit tests first, then read the one that fails.";
const char* const kToolsAfter =
    "One test fails: `store_unit_compaction` expects the newer generation after a crash between the rename and the "
    "flush, and gets the older one. That is the corner from Monday's plan, so the test is right and the reader is "
    "wrong. Everything else passes (412 tests).";

const char* const kStoppedText =
    "# Design: the journal's second generation\n\n"
    "## Why\n\n"
    "Compaction today writes the new generation beside the old one and renames it into place. ";

const char* const kErrorText = "Deploying to the bench now.";
const char* const kCutText = "Last night's logs show three restarts of the server, all between two and three in the";

// The replies to new prompts, in turn.
const char* const kReplyFirst =
    "Got it. Here's what I'd do: read the failing test, find the smallest change that makes it pass, and add a test "
    "that fails without that change.";
const char* const kReplyList =
    "Three things stand out:\n\n"
    "1. **The store** keeps two generations after a crash.\n"
    "2. **The link** resends actions to the old server during a handover.\n"
    "3. **The drivers** report a vanished harness as a person's mistake.\n\n"
    "I'd take them in that order.";
const char* const kReplyCode =
    "A small fix, with the test that shows it:\n\n"
    "```cpp\n"
    "if (pending_.size() + chunk.size() > kMaxLine) return refuse(\"line longer than the limit\");\n"
    "```\n\n"
    "Shall I open a pull request?";

std::string emitText(const std::string& text) {
    return "emit {\"kind\":\"text\",\"delta\":" + jsonQuote(text) + "}\n";
}
std::string stream(int ms, const std::string& text) {
    return "stream " + std::to_string(ms) + " " + jsonQuote(text) + "\n";
}
std::string tool(const std::string& name, const std::string& summary) {
    return "emit {\"kind\":\"tool\",\"name\":" + jsonQuote(name) + ",\"summary\":" + jsonQuote(summary) + "}\n";
}
std::string done(const char* outcome) {
    return std::string("emit {\"kind\":\"done\",\"outcome\":\"") + outcome + "\"}\n";
}

} // namespace

const std::vector<std::string>& names() {
    static const std::vector<std::string> kNames = {"Ada", "Grace", "Linus", "Margaret", "Alan", "Barbara", "Edsger",
        "Frances", "Donald", "Hedy", "Katherine", "Dennis", "Ken", "Radia", "Bjarne", "Guido", "Niklaus", "Tony",
        "Leslie", "Shafi", "Sophie", "Annie", "Jean", "Butler", "Dorothy", "Joan", "Evelyn", "Adele", "Seymour", "Vint"};
    return kNames;
}

bool aisFor(std::size_t count, std::vector<std::string>* out, std::string* why) {
    if (count > kMaxAis) {
        *why = "the mock makes at most " + std::to_string(kMaxAis) + " AIs (a server holds no more)";
        return false;
    }
    out->clear();
    for (std::size_t i = 0; i < count; ++i)
        out->push_back(i < names().size() ? names()[i] : "mock-" + std::to_string(i + 1));
    return true;
}

bool aisFrom(const std::string& list, std::vector<std::string>* out, std::string* why) {
    out->clear();
    std::set<std::string> seen;
    std::size_t at = 0;
    while (at <= list.size()) {
        std::size_t comma = list.find(',', at);
        if (comma == std::string::npos)
            comma = list.size();
        const std::string name = list.substr(at, comma - at);
        if (!accounts::isName(name)) {
            *why = "\"" + name + "\" is not an AI's name: 1 to 64 letters, digits, '-', '_' or '.', not starting with "
                "'.' or '-'";
            return false;
        }
        if (name == kSignInName) {
            *why = std::string(kSignInName) + " is the sign-in preset's AI (--sign-in); give the others other names";
            return false;
        }
        if (!seen.insert(name).second) {
            *why = name + " is named twice";
            return false;
        }
        out->push_back(name);
        at = comma + 1;
    }
    if (out->size() > kMaxAis) {
        *why = "the mock makes at most " + std::to_string(kMaxAis) + " AIs (a server holds no more)";
        return false;
    }
    return true;
}

std::vector<SeedTurn> seedFor(const std::string& name, std::size_t index) {
    using E = SeedTurn::Ends;
    const SeedTurn hi{"Hi " + name + ", are you there?", E::Ok, {}, "Here and ready. What are we working on today?"};
    const SeedTurn plan{"What's the plan for this week?", E::Ok, {}, kLong};
    const SeedTurn checklist{"Can you outline the release checklist?", E::Ok, {}, kMarkdown};
    const SeedTurn code{"Show me the parser's main loop.", E::Ok, {}, kCode};
    const SeedTurn tests{"Run the unit tests and tell me what failed.", E::Ok, {}, kToolsBefore};
    const SeedTurn stopped{"Write the whole design doc now.", E::Stopped, "Compaction today", kStoppedText};
    const SeedTurn error{"Deploy it to the bench.", E::Error, {}, kErrorText};
    const SeedTurn cut{"Summarize last night's logs.", E::Cut, {}, kCutText};
    const SeedTurn back{"Are you back?", E::Ok, {}, "Back. My driver restarted, and the conversation is all still here."};
    if (index % 2 == 0)
        return {hi, stopped, error, cut, back, tests, plan, checklist, code};
    return {hi, plan, checklist, code, tests, stopped, error, cut, back};
}

std::string firstReplyText() {
    return kReplyFirst;
}

std::string scriptFor(const std::string& name, bool instant) {
    const int think = instant ? 0 : kThinkMs;
    const int word = instant ? 0 : kWordMs;
    const int toolMs = instant ? 0 : kToolMs;
    std::ostringstream s;
    s << "# " << name << ", one of the mock's fake AIs (tests/mock/mock.h). Written by nexus-mock start.\n"
      << "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"mock\"}\n"
      << "emit {\"kind\":\"session\",\"id\":" << jsonQuote("mock-" + name) << "}\n"
      << "option cycle\n";
    // The seeded turns, written at once: the seeder plays them before any client connects.
    for (const SeedTurn& t : seedFor(name, 0)) {
        s << "\nprompt " << t.prompt << "\n";
        switch (t.ends) {
        case SeedTurn::Ends::Ok:
            s << stream(0, t.reply);
            if (t.reply == kToolsBefore)
                s << tool("Bash", "ctest --test-dir build -L unit -j 8 --output-on-failure")
                  << tool("Read", "tests/store/compaction_test.cpp") << stream(0, kToolsAfter);
            s << done("ok");
            break;
        case SeedTurn::Ends::Stopped:
            s << stream(0, t.reply) << "await-stop\n" << done("stopped");
            break;
        case SeedTurn::Ends::Error:
            s << stream(0, t.reply) << tool("Bash", "nova-reserve run -- ./deploy.sh bench")
              << "emit {\"kind\":\"error\",\"cause\":\"the bench refused the connection (a scripted error: this is the "
                 "mock)\",\"fix\":\"send the prompt again\",\"code\":\"harness\"}\n"
              << done("error");
            break;
        case SeedTurn::Ends::Cut:
            s << emitText(t.reply) << "exit 1\n";
            break;
        }
    }
    // Any other prompt: these, in turn, round and round.
    s << "\nsend\npause " << think << "\n" << stream(word, kReplyFirst) << done("ok");
    s << "\nsend\npause " << think << "\n" << tool("Grep", "pending_ src/parser.cpp") << "pause " << toolMs << "\n"
      << stream(word, kReplyList) << done("ok");
    s << "\nsend\npause " << think << "\n" << stream(word, kReplyCode) << done("ok");
    return s.str();
}

std::string driversFor(const Options& o, const std::string& fake, const std::string& scripts, const std::string& work) {
    std::ostringstream s;
    s << "# The mock's AIs (tests/mock/mock.h), each on the fake driver with a script of its own.\n";
    for (const std::string& name : o.ais) {
        s << "driver mock-" << name << " " << fake << " --script " << scripts << "/" << name << ".script\n"
          << "ai " << name << " mock-" << name << " " << work << "/" << name << "\n";
    }
    if (o.signIn) {
        s << "driver claude " << fake << " --script " << scripts << "/" << kSignInName << ".script\n"
          << "ai " << kSignInName << " claude " << work << "/" << kSignInName << "\n";
    }
    return s.str();
}

accounts::Registry registryFor(const Options& o, const std::string& work) {
    accounts::Registry r;
    for (const std::string& name : o.ais) {
        accounts::AI f{name, {}, {}};
        f.instances = {{"mock", kMachine, work + "/" + name}};
        f.limit = false;
        r.ais.push_back(std::move(f));
    }
    if (o.signIn) {
        r.accounts.push_back(accounts::Account{kSignInAccount, "claude"});
        accounts::AI f{kSignInName, {kSignInAccount}, kSignInAccount};
        f.instances = {{"mock", kMachine, work + "/" + kSignInName}};
        f.limit = false;
        r.ais.push_back(std::move(f));
    }
    return r;
}

std::string serverConfigFor(const Options& o, const std::string& claude) {
    std::string s = std::string("machine ") + kMachine + "\n";
    if (o.signIn)
        s += "claude " + claude + "\n";
    return s;
}

bool serverCommandFor(const std::string& server, const std::string& home, const std::string& dir, std::string* out,
    std::string* why) {
    for (const std::string* p : {&server, &home, &dir}) {
        if (p->empty() || p->front() != '/' || p->find_first_of(" \t\n") != std::string::npos) {
            *why = "\"" + *p + "\" is not an absolute path without white space, which a client's --server-command needs";
            return false;
        }
    }
    *out = "/usr/bin/env HOME=" + home + " " + server + " new --dir " + dir;
    return true;
}

std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* const kHex = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(c >> 4) & 0xf];
                out += kHex[c & 0xf];
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

} // namespace nexus::mock
