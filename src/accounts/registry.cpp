// The accounts and friends (registry.h): their rules, their files' text, and the accounts the server picks.

#include "accounts/registry.h"
#include "policy/host.h"

#include <cstdio>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>

#include "pr/json.h"

namespace nexus::accounts {

namespace {

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

std::string_view view(PrStringView s) {
    return s.data == nullptr ? std::string_view() : std::string_view(s.data, s.size);
}

// A JSON string, escaped as Prax's writer escapes it.
std::string quoted(std::string_view s) {
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterString(w, pr(s));
    std::string out(view(prJsonWriterText(w)));
    prJsonWriterDestroy(w);
    return out;
}

// The line (from 1) of byte `at` in `text`.
std::size_t lineOf(std::string_view text, std::size_t at) {
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() +
        static_cast<std::ptrdiff_t>(std::min(at, text.size())), '\n'));
}

// The byte where each value of a document starts, in Prax's document order (a container, then its contents; an
// object's member as its key, then its value), so that an error about a value can name its line. The text has
// already been read by Prax, so it is JSON.
std::vector<std::size_t> starts(std::string_view t) {
    std::vector<std::size_t> out;
    std::size_t i = 0;
    while (i < t.size()) {
        const char c = t[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ':' || c == ']' || c == '}') {
            ++i;
        } else if (c == '"') {
            out.push_back(i);
            for (++i; i < t.size() && t[i] != '"'; ++i) {
                if (t[i] == '\\') {
                    ++i;
                }
            }
            ++i;
        } else if (c == '[' || c == '{') {
            out.push_back(i);
            ++i;
        } else if (static_cast<unsigned char>(c) == 0xef && i == 0) {
            i += 3;     // a byte order mark
        } else {
            out.push_back(i);
            while (i < t.size() && t[i] != ',' && t[i] != ']' && t[i] != '}' && t[i] != ' ' && t[i] != '\n' &&
                   t[i] != '\r' && t[i] != '\t') {
                ++i;
            }
        }
    }
    return out;
}

const char* kindName(PrJsonKind k) {
    switch (k) {
        case PrJsonKind_Null: return "null";
        case PrJsonKind_False:
        case PrJsonKind_True: return "true or false";
        case PrJsonKind_Number: return "a number";
        case PrJsonKind_String: return "a string";
        case PrJsonKind_Array: return "a list";
        default: return "an object";
    }
}

// One file's reading: the document, where its values start, and the errors found.
class Reader {
public:
    Reader(std::string_view text, const std::string& path) : text_(text), path_(path), doc_(prJsonCreate()) {}
    ~Reader() { prJsonDestroy(doc_); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool parse() {
        std::size_t at = 0;
        if (!prJsonParse(doc_, pr(text_), &at)) {
            error(at, "this is not JSON (a missing comma, quote or bracket, or a trailing comma, near here)",
                  "fix the text at this line; docs/accounts.md has an example of the whole file");
            return false;
        }
        starts_ = starts(text_);
        return true;
    }

    void error(std::size_t at, const std::string& what, const std::string& fix) {
        read.errors.push_back(path_ + ":" + std::to_string(lineOf(text_, at)) + ": " + what + "; " + fix);
    }
    void errorAt(PrJsonValueIndex v, const std::string& what, const std::string& fix) {
        error(v < starts_.size() ? starts_[v] : 0, what, fix);
    }

    PrJson* doc() { return doc_; }
    Read read;

private:
    std::string_view text_;
    std::string path_;
    PrJson* doc_;
    std::vector<std::size_t> starts_;
};

// The object at `object`'s members: refuses any key not in `allowed`, and a key twice.
bool onlyKeys(Reader& r, PrJsonValueIndex object, const std::vector<std::string>& allowed, const std::string& what) {
    bool ok = true;
    std::set<std::string> seen;
    for (PrJsonValueIndex k = prJsonFirst(r.doc(), object); k != kPrJsonMissing; k = prJsonAfter(r.doc(), object, k)) {
        const std::string key(view(prJsonString(r.doc(), k)));
        std::string list;
        for (const std::string& a : allowed) {
            list += (list.empty() ? "\"" : ", \"") + a + "\"";
        }
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            r.errorAt(k, what + " has a field \"" + key + "\", which is not one of its fields (" + list + ")",
                      "remove it, or correct its spelling");
            ok = false;
        } else if (!seen.insert(key).second) {
            r.errorAt(k, what + " has the field \"" + key + "\" twice", "keep one");
            ok = false;
        }
    }
    return ok;
}

// The top: an object of "version", the one list `list`, and the optional fields `more`. Returns the list's index, or
// kPrJsonMissing.
PrJsonValueIndex top(Reader& r, const std::string& list, const std::string& example,
                     const std::vector<std::string>& more = {}, int maxVersion = kVersion) {
    if (prJsonKind(r.doc(), 0) != PrJsonKind_Object) {
        r.errorAt(0, "the file is " + std::string(kindName(prJsonKind(r.doc(), 0))) + ", not an object",
                  "make it an object of \"version\" and \"" + list + "\", as " + example);
        return kPrJsonMissing;
    }
    std::vector<std::string> keys = {"version", list};
    keys.insert(keys.end(), more.begin(), more.end());
    bool ok = onlyKeys(r, 0, keys, "the file");
    const PrJsonValueIndex v = prJsonMember(r.doc(), 0, pr("version"));
    if (v == kPrJsonMissing) {
        r.errorAt(0, "the file has no \"version\"", "add \"version\": " + std::to_string(kVersion) + " at its top");
        ok = false;
    } else {
        const double n = prJsonNumber(r.doc(), v, -1);
        if (prJsonKind(r.doc(), v) != PrJsonKind_Number || n < 1 || n != std::floor(n)) {
            r.errorAt(v, "\"version\" is not a whole number from 1", "set it to " + std::to_string(kVersion));
            ok = false;
        } else if (n > maxVersion) {
            // n is range-checked before the cast: 1e300 is a number JSON allows, and no long long holds it
            const std::string said = n < 1e9 ? std::to_string(static_cast<long long>(n))
                                             : std::string("a number past any version");
            r.errorAt(v, "\"version\" is " + said + ", newer than this " +
                      "nexus-server reads (" + std::to_string(maxVersion) + ")",
                      "run the nexus-server that wrote it, or update this one; the file was left as it is");
            ok = false;
        } else {
            r.read.version = static_cast<int>(n);
            // The upgrades, in the order of the versions, are in the fields' defaults: version 1 had no "billing"
            // (every account a subscription's) and no "limit" (every friend unlimited), and its file is read so.
            r.read.upgraded = r.read.version < kVersion;
        }
    }
    const PrJsonValueIndex l = prJsonMember(r.doc(), 0, pr(list));
    if (l == kPrJsonMissing) {
        r.errorAt(0, "the file has no \"" + list + "\"", "add \"" + list + "\": [] (or the list) at its top");
        return kPrJsonMissing;
    }
    if (prJsonKind(r.doc(), l) != PrJsonKind_Array) {
        r.errorAt(l, "\"" + list + "\" is " + kindName(prJsonKind(r.doc(), l)) + ", not a list",
                  "make it a list, [ ... ], as " + example);
        return kPrJsonMissing;
    }
    return ok ? l : kPrJsonMissing;
}

// A string member: true with its value; false (an error made) when present and not a string, or absent and required.
bool stringField(Reader& r, PrJsonValueIndex object, const std::string& key, const std::string& what, bool required,
                 bool nullable, std::string* out) {
    const PrJsonValueIndex v = prJsonMember(r.doc(), object, pr(key));
    if (v == kPrJsonMissing || (nullable && prJsonKind(r.doc(), v) == PrJsonKind_Null)) {
        if (required) {
            r.errorAt(object, what + " has no \"" + key + "\"", "add \"" + key + "\": \"...\"");
            return false;
        }
        out->clear();
        return true;
    }
    if (prJsonKind(r.doc(), v) != PrJsonKind_String) {
        r.errorAt(v, what + "'s \"" + key + "\" is " + kindName(prJsonKind(r.doc(), v)) + ", not a string",
                  std::string("put it in double quotes") + (nullable ? ", or make it null for none" : ""));
        return false;
    }
    *out = std::string(view(prJsonString(r.doc(), v)));
    return true;
}

// A true-or-false member: true with its value (`fallback` when absent); false (an error made) when not true or false.
bool boolField(Reader& r, PrJsonValueIndex object, const std::string& key, const std::string& what, bool fallback,
               bool* out) {
    const PrJsonValueIndex v = prJsonMember(r.doc(), object, pr(key));
    if (v == kPrJsonMissing) {
        *out = fallback;
        return true;
    }
    const PrJsonKind k = prJsonKind(r.doc(), v);
    if (k != PrJsonKind_True && k != PrJsonKind_False) {
        r.errorAt(v, what + "'s \"" + key + "\" is " + kindName(k) + ", not true or false",
                  "write true or false, without quotes");
        return false;
    }
    *out = k == PrJsonKind_True;
    return true;
}

const char* const kNameRule ="1 to 64 letters, digits, '-', '_' or '.', not starting with '.' or '-'";
const char* const kProviderRule = "1 to 32 lowercase letters, digits or '-', as \"claude\"";
const char* const kMachineRule = "1 to 253 letters, digits, '-' or '.', not starting with '-' or '.', as \"studio\"";
const char* const kDirRule = "starting with '/', printable, at most 4096 bytes, as \"/Users/me/work\"";
const char* const kAccountsExample =
    "{ \"version\": 2, \"accounts\": [ { \"name\": \"work\", \"provider\": \"claude\", \"billing\": \"subscription\" } ] }";
const char* const kFriendsExample =
    "{ \"version\": 2, \"friends\": [ { \"name\": \"claude\", \"limit\": true, \"accounts\": [\"work\"], \"default\": "
    "\"work\" } ] }";

bool contains(const std::vector<std::string>& v, std::string_view x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// A "limit_keeps_out" directory: an absolute directory (isDir), never the root itself, which would keep a limited
// friend out of its own directory too.
bool isKeptOut(std::string_view dir) {
    return isDir(dir) && dir != "/";
}
const char* const kKeptOutRule = "an absolute directory other than \"/\", printable, at most 4096 bytes, as \"/data\"";

} // namespace

const Account* Registry::account(std::string_view name) const {
    for (const Account& a : accounts) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

const Friend* Registry::friendNamed(std::string_view name) const {
    for (const Friend& f : friends) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

Friend* Registry::friendNamed(std::string_view name) {
    for (Friend& f : friends) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

const Instance* Friend::instance(std::string_view instanceName) const {
    for (const Instance& i : instances) {
        if (i.name == instanceName) {
            return &i;
        }
    }
    return nullptr;
}

bool Registry::linked(std::string_view friendName, std::string_view account) const {
    const Friend* f = friendNamed(friendName);
    return f != nullptr && contains(f->accounts, account);
}

bool isAccountText(std::string_view value) {
    return value.size() <= 254 && std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

bool isName(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == '.' || name.front() == '-') {   // '-': it reaches an argv
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
               c == '.';
    });
}

bool isProvider(std::string_view provider) {
    if (provider.empty() || provider.size() > 32 || provider.front() == '-') {
        return false;
    }
    return std::all_of(provider.begin(), provider.end(),
        [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool isMachine(std::string_view machine) {
    if (machine.empty() || machine.size() > 253 || machine.front() == '.' || machine.front() == '-') {
        return false;
    }
    return std::all_of(machine.begin(), machine.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    });
}

bool isDir(std::string_view dir) {
    if (dir.empty() || dir.size() > 4096 || dir.front() != '/') {
        return false;
    }
    return std::all_of(dir.begin(), dir.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f; });
}

std::vector<std::string> problems(const Registry& r) {
    std::vector<std::string> out;
    std::set<std::string> names;
    for (const Account& a : r.accounts) {
        if (!isName(a.name)) {
            out.push_back("accounts.json: the account \"" + a.name + "\" is not a name (" + kNameRule + "); rename it");
        }
        if (!isProvider(a.provider)) {
            out.push_back("accounts.json: the account \"" + a.name + "\" has the provider \"" + a.provider +
                          "\", which is not one (" + kProviderRule + "); correct it");
        }
        if (!isAccountText(a.label)) {
            out.push_back("accounts.json: the label for account " + a.name +
                          " must be one line of at most 254 bytes; shorten it or remove control characters");
        }
        if (!isAccountText(a.email)) {
            out.push_back("accounts.json: the email for account " + a.name +
                          " must be one line of at most 254 bytes; shorten it or remove control characters");
        }
        if (!names.insert(a.name).second) {
            out.push_back("accounts.json: the account \"" + a.name + "\" is there twice; keep one");
        }
    }
    std::set<std::string> friends;
    for (const Friend& f : r.friends) {
        if (!isName(f.name)) {
            out.push_back("friends.json: the friend \"" + f.name + "\" is not a name (" + kNameRule + "); rename it");
        }
        if (!friends.insert(f.name).second) {
            out.push_back("friends.json: the friend \"" + f.name + "\" is there twice; keep one");
        }
        std::set<std::string> links;
        for (const std::string& a : f.accounts) {
            if (r.account(a) == nullptr) {
                out.push_back("friends.json: the friend \"" + f.name + "\" links to the account \"" + a + "\", which " +
                              "accounts.json does not set up; add the account there, or take it out of \"" + f.name +
                              "\"'s \"accounts\"");
            }
            if (!links.insert(a).second) {
                out.push_back("friends.json: the friend \"" + f.name + "\" links to the account \"" + a + "\" twice; " +
                              "keep one");
            }
        }
        // A session on an account billed at API rate is always limited: so its friend may not say otherwise.
        for (const std::string& a : f.accounts) {
            const Account* acc = r.account(a);
            if (!f.limit && acc != nullptr && acc->billing == Billing::Api) {
                out.push_back("friends.json: the friend \"" + f.name + "\" has \"limit\": false and links to the "
                              "account \"" + a + "\", which accounts.json bills at API rate (\"billing\": \"api\"); a "
                              "friend on an API-billed account is always limited to its directory, so set \"" +
                              f.name + "\"'s \"limit\" to true, or take \"" + a + "\" out of its \"accounts\"");
            }
        }
        if (!f.preferred.empty() && !contains(f.accounts, f.preferred)) {
            out.push_back("friends.json: the friend \"" + f.name + "\"'s \"default\" is \"" + f.preferred + "\", " +
                          "which is not in its \"accounts\"; add it to \"accounts\", or set \"default\" to one of " +
                          "them or to null");
        }
        std::set<std::string> instances;
        for (const Instance& i : f.instances) {
            const std::string named = "friends.json: the friend \"" + f.name + "\"'s instance \"" + i.name + "\"";
            if (!isName(i.name)) {
                out.push_back(named + " is not a name (" + kNameRule + "); rename it");
            }
            if (!instances.insert(i.name).second) {
                out.push_back(named + " is there twice; keep one, or rename one");
            }
            if (!isMachine(i.machine)) {
                out.push_back(named + " has the machine \"" + i.machine + "\", which is not a host name (" +
                              kMachineRule + "); correct it");
            }
            if (!isDir(i.dir)) {
                out.push_back(named + " has the dir \"" + i.dir + "\", which is not an absolute path (" + kDirRule +
                              "); correct it");
            }
        }
    }
    std::set<std::string> kept;
    for (const std::string& d : r.keepsOut) {
        if (!isKeptOut(d)) {
            out.push_back("friends.json: \"limit_keeps_out\" holds \"" + d + "\", which is not " + kKeptOutRule +
                          "; correct it, or take it out");
        }
        if (!kept.insert(d).second) {
            out.push_back("friends.json: \"limit_keeps_out\" holds \"" + d + "\" twice; keep one");
        }
    }
    return out;
}

std::string accountsText(const Registry& r) {
    const bool usesOptions = std::any_of(r.accounts.begin(), r.accounts.end(), [](const Account& a) {
        return !a.label.empty() || !a.email.empty() || a.signInOnUse;
    });
    const int version = usesOptions ? kAccountOptionsVersion : kVersion;
    std::string s = "{\n  \"version\": " + std::to_string(version) + ",\n  \"accounts\": [";
    for (std::size_t i = 0; i < r.accounts.size(); ++i) {
        s += std::string(i == 0 ? "\n" : ",\n") + "    {\n      \"name\": " + quoted(r.accounts[i].name) +
             ",\n      \"provider\": " + quoted(r.accounts[i].provider) + ",\n      \"billing\": " +
             quoted(billingName(r.accounts[i].billing));
        if (!r.accounts[i].label.empty()) {
            s += ",\n      \"label\": " + quoted(r.accounts[i].label);
        }
        if (!r.accounts[i].email.empty()) {
            s += ",\n      \"email\": " + quoted(r.accounts[i].email);
        }
        if (r.accounts[i].signInOnUse) {
            s += ",\n      \"sign_in_on_use\": true";
        }
        s += "\n    }";
    }
    s += r.accounts.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return s;
}

std::string friendsText(const Registry& r) {
    std::string s = "{\n  \"version\": " + std::to_string(kVersion) + ",\n";
    // Written only when it holds a directory, so a file without the field is saved as it was.
    if (!r.keepsOut.empty()) {
        s += "  \"limit_keeps_out\": [";
        for (std::size_t k = 0; k < r.keepsOut.size(); ++k) {
            s += std::string(k == 0 ? "\n" : ",\n") + "    " + quoted(r.keepsOut[k]);
        }
        s += "\n  ],\n";
    }
    s += "  \"friends\": [";
    for (std::size_t i = 0; i < r.friends.size(); ++i) {
        const Friend& f = r.friends[i];
        s += std::string(i == 0 ? "\n" : ",\n") + "    {\n      \"name\": " + quoted(f.name) +
             ",\n      \"limit\": " + (f.limit ? "true" : "false") + ",\n      \"accounts\": [";
        for (std::size_t k = 0; k < f.accounts.size(); ++k) {
            s += std::string(k == 0 ? "\n" : ",\n") + "        " + quoted(f.accounts[k]);
        }
        s += f.accounts.empty() ? "],\n" : "\n      ],\n";
        s += "      \"default\": " + (f.preferred.empty() ? std::string("null") : quoted(f.preferred)) + ",\n";
        s += "      \"instances\": [";
        for (std::size_t k = 0; k < f.instances.size(); ++k) {
            const Instance& in = f.instances[k];
            s += std::string(k == 0 ? "\n" : ",\n") + "        {\n          \"name\": " + quoted(in.name) +
                 ",\n          \"machine\": " + quoted(in.machine) + ",\n          \"dir\": " + quoted(in.dir) +
                 "\n        }";
        }
        s += f.instances.empty() ? "]\n    }" : "\n      ]\n    }";
    }
    s += r.friends.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return s;
}

Read readAccounts(std::string_view text, const std::string& path, Registry* registry) {
    registry->accounts.clear();
    Reader r(text, path);
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        r.read.ok = true;
        r.read.version = kVersion;
        return r.read;
    }
    if (!r.parse()) {
        return r.read;
    }
    const PrJsonValueIndex list = top(r, "accounts", kAccountsExample, {}, kAccountOptionsVersion);
    if (list == kPrJsonMissing) {
        return r.read;
    }
    for (PrJsonValueIndex e = prJsonFirst(r.doc(), list); e != kPrJsonMissing; e = prJsonAfter(r.doc(), list, e)) {
        if (prJsonKind(r.doc(), e) != PrJsonKind_Object) {
            r.errorAt(e, "an account is " + std::string(kindName(prJsonKind(r.doc(), e))) + ", not an object",
                      std::string("write each account as ") + "{ \"name\": \"work\", \"provider\": \"claude\" }");
            continue;
        }
        Account a;
        const std::string what = "an account";
        bool ok = onlyKeys(r, e, {"name", "provider", "billing", "label", "sign_in_on_use", "email"}, what);
        ok = stringField(r, e, "name", what, true, false, &a.name) && ok;
        const std::string named = a.name.empty() ? what : "the account \"" + a.name + "\"";
        ok = stringField(r, e, "provider", named, true, false, &a.provider) && ok;
        ok = stringField(r, e, "label", named, false, false, &a.label) && ok;
        ok = stringField(r, e, "email", named, false, false, &a.email) && ok;
        if (!isAccountText(a.email)) {
            r.errorAt(prJsonMember(r.doc(), e, pr("email")), named + "'s email must be one line of at most 254 bytes",
                      "shorten it or remove control characters");
            ok = false;
        }
        ok = boolField(r, e, "sign_in_on_use", named, false, &a.signInOnUse) && ok;
        if (!isAccountText(a.label)) {
            r.errorAt(prJsonMember(r.doc(), e, pr("label")), named + "'s label must be one line of at most 254 bytes",
                      "shorten it or remove control characters");
            ok = false;
        }
        // Absent: a subscription's, in every version (version 1 had no API-billed accounts).
        std::string billing;
        ok = stringField(r, e, "billing", named, false, false, &billing) && ok;
        if (billing == "api") {
            a.billing = Billing::Api;
        } else if (!billing.empty() && billing != "subscription") {
            r.errorAt(prJsonMember(r.doc(), e, pr("billing")), named + "'s \"billing\" is \"" + billing +
                      "\", which is not one of \"subscription\" and \"api\"", "write \"subscription\" (a "
                      "subscription's login) or \"api\" (billed at the provider's API rate)");
            ok = false;
        }
        if (ok && !isName(a.name)) {
            r.errorAt(e, named + " is not a name (" + kNameRule + ")", "rename it");
            ok = false;
        }
        if (ok && !isProvider(a.provider)) {
            r.errorAt(e, named + "'s provider \"" + a.provider + "\" is not one (" + kProviderRule + ")",
                      "correct it");
            ok = false;
        }
        if (ok) {
            registry->accounts.push_back(std::move(a));
        }
    }
    r.read.ok = r.read.errors.empty();
    return r.read;
}

Read readFriends(std::string_view text, const std::string& path, Registry* registry, const Registry* committed) {
    registry->friends.clear();
    registry->keepsOut.clear();
    Reader r(text, path);
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        r.read.ok = true;
        r.read.version = kVersion;
        return r.read;
    }
    if (!r.parse()) {
        return r.read;
    }
    const PrJsonValueIndex list = top(r, "friends", kFriendsExample, {"limit_keeps_out"});
    if (list == kPrJsonMissing) {
        return r.read;
    }
    // "limit_keeps_out": absent is none. Each a directory a limited friend's AI is kept out of (isKeptOut).
    const PrJsonValueIndex ko = prJsonMember(r.doc(), 0, pr("limit_keeps_out"));
    if (ko != kPrJsonMissing && prJsonKind(r.doc(), ko) != PrJsonKind_Array) {
        r.errorAt(ko, "\"limit_keeps_out\" is " + std::string(kindName(prJsonKind(r.doc(), ko))) + ", not a list",
                  "make it a list of absolute directories, as [\"/data\"], or take it out for none");
    } else if (ko != kPrJsonMissing) {
        std::set<std::string> seen;
        for (PrJsonValueIndex d = prJsonFirst(r.doc(), ko); d != kPrJsonMissing; d = prJsonAfter(r.doc(), ko, d)) {
            if (prJsonKind(r.doc(), d) != PrJsonKind_String) {
                r.errorAt(d, "\"limit_keeps_out\" holds " + std::string(kindName(prJsonKind(r.doc(), d))) +
                          ", not a directory", "put each directory in double quotes, as \"/data\"");
                continue;
            }
            std::string dir(view(prJsonString(r.doc(), d)));
            if (!isKeptOut(dir)) {
                r.errorAt(d, "\"limit_keeps_out\" holds \"" + dir + "\", which is not " + kKeptOutRule,
                          "correct it, or take it out");
            } else if (!seen.insert(dir).second) {
                r.errorAt(d, "\"limit_keeps_out\" holds \"" + dir + "\" twice", "keep one");
            } else {
                registry->keepsOut.push_back(std::move(dir));
            }
        }
    }
    std::size_t count = 0;
    for (PrJsonValueIndex e = prJsonFirst(r.doc(), list); e != kPrJsonMissing; e = prJsonAfter(r.doc(), list, e)) {
        ++count;
    }
    if (count > kMaxFriends) {
        r.errorAt(list, "\"friends\" holds " + std::to_string(count) + " friends, and nexus-server takes at most " +
                  std::to_string(kMaxFriends), "take friends out until it holds " + std::to_string(kMaxFriends) +
                  " or fewer; nothing was changed");
        return r.read;
    }
    for (PrJsonValueIndex e = prJsonFirst(r.doc(), list); e != kPrJsonMissing; e = prJsonAfter(r.doc(), list, e)) {
        if (prJsonKind(r.doc(), e) != PrJsonKind_Object) {
            r.errorAt(e, "a friend is " + std::string(kindName(prJsonKind(r.doc(), e))) + ", not an object",
                      std::string("write each friend as ") +
                      "{ \"name\": \"claude\", \"accounts\": [\"work\"], \"default\": \"work\" }");
            continue;
        }
        Friend f;
        bool ok = onlyKeys(r, e, {"name", "limit", "accounts", "default", "instances"}, "a friend");
        ok = stringField(r, e, "name", "a friend", true, false, &f.name) && ok;
        const std::string named = f.name.empty() ? std::string("a friend") : "the friend \"" + f.name + "\"";
        // Absent: limited, for a friend written since the field (version 2); not limited in a version 1 file, whose
        // friends ran unlimited, so that nothing changes under a person until the file says so. A friend the committed
        // files do not hold never ran: limited, whatever the file's version.
        const bool known = committed == nullptr || committed->friendNamed(f.name) != nullptr;
        ok = boolField(r, e, "limit", named, r.read.version >= 2 || !known, &f.limit) && ok;
        ok = stringField(r, e, "default", named, false, true, &f.preferred) && ok;
        const PrJsonValueIndex l = prJsonMember(r.doc(), e, pr("accounts"));
        if (l != kPrJsonMissing && prJsonKind(r.doc(), l) != PrJsonKind_Array) {
            r.errorAt(l, named + "'s \"accounts\" is " + kindName(prJsonKind(r.doc(), l)) + ", not a list",
                      "make it a list of account names, as [\"work\", \"home\"], or [] for none");
            ok = false;
        } else if (l != kPrJsonMissing) {
            for (PrJsonValueIndex a = prJsonFirst(r.doc(), l); a != kPrJsonMissing; a = prJsonAfter(r.doc(), l, a)) {
                if (prJsonKind(r.doc(), a) != PrJsonKind_String) {
                    r.errorAt(a, named + "'s \"accounts\" holds " + kindName(prJsonKind(r.doc(), a)) +
                              ", not an account's name", "put each account's name in double quotes");
                    ok = false;
                } else {
                    f.accounts.emplace_back(view(prJsonString(r.doc(), a)));
                }
            }
        }
        const PrJsonValueIndex is = prJsonMember(r.doc(), e, pr("instances"));
        if (is != kPrJsonMissing && prJsonKind(r.doc(), is) != PrJsonKind_Array) {
            r.errorAt(is, named + "'s \"instances\" is " + kindName(prJsonKind(r.doc(), is)) + ", not a list",
                      "make it a list of instances, as [{ \"name\": \"studio\", \"machine\": \"studio\", \"dir\": "
                      "\"/Users/me/work\" }], or [] for none");
            ok = false;
        } else if (is != kPrJsonMissing) {
            for (PrJsonValueIndex x = prJsonFirst(r.doc(), is); x != kPrJsonMissing; x = prJsonAfter(r.doc(), is, x)) {
                if (prJsonKind(r.doc(), x) != PrJsonKind_Object) {
                    r.errorAt(x, named + "'s \"instances\" holds " + kindName(prJsonKind(r.doc(), x)) +
                              ", not an instance", "write each instance as { \"name\": \"studio\", \"machine\": "
                              "\"studio\", \"dir\": \"/Users/me/work\" }");
                    ok = false;
                    continue;
                }
                Instance in;
                const std::string what = "an instance of " + named;
                bool good = onlyKeys(r, x, {"name", "machine", "dir"}, what);
                good = stringField(r, x, "name", what, true, false, &in.name) && good;
                const std::string inst = in.name.empty() ? what : named + "'s instance \"" + in.name + "\"";
                good = stringField(r, x, "machine", inst, true, false, &in.machine) && good;
                good = stringField(r, x, "dir", inst, true, false, &in.dir) && good;
                if (good && !isName(in.name)) {
                    r.errorAt(x, inst + " is not a name (" + kNameRule + ")", "rename it");
                    good = false;
                }
                if (good && !isMachine(in.machine)) {
                    r.errorAt(x, inst + "'s machine \"" + in.machine + "\" is not a host name (" + kMachineRule + ")",
                              "correct it");
                    good = false;
                }
                if (good && !isDir(in.dir)) {
                    r.errorAt(x, inst + "'s dir \"" + in.dir + "\" is not an absolute path (" + kDirRule + ")",
                              "correct it");
                    good = false;
                }
                if (good) {
                    f.instances.push_back(std::move(in));
                }
                ok = good && ok;
            }
        }
        if (ok && !isName(f.name)) {
            r.errorAt(e, named + " is not a name (" + kNameRule + ")", "rename it");
            ok = false;
        }
        if (ok) {
            registry->friends.push_back(std::move(f));
        }
    }
    r.read.ok = r.read.errors.empty();
    return r.read;
}

std::string nextUp(const Registry& registry, const std::set<std::string>& up, std::string_view friendName,
                   std::string_view current) {
    const Friend* f = registry.friendNamed(friendName);
    if (f == nullptr || f->accounts.empty()) {
        return std::string();
    }
    std::vector<std::string> eligible;
    for (const auto& account : f->accounts) {
        if (account != current && up.count(account))
            eligible.push_back(account);
    }
    policy::Host stock("server");
    std::string why;
    auto selected = policy::accountAfterLimit(stock, {}, std::string(current), f->accounts, eligible, why);
    if (!why.empty())
        std::fprintf(stderr, "nexus: shipped account policy: %s\n", why.c_str());
    return selected;
}

std::string landing(const Registry& registry, const std::set<std::string>& up, std::string_view friendName) {
    const Friend* f = registry.friendNamed(friendName);
    if (f == nullptr) {
        return std::string();
    }
    if (!f->preferred.empty() && up.count(f->preferred) != 0) {
        return f->preferred;
    }
    for (const std::string& a : f->accounts) {
        if (up.count(a) != 0) {
            return a;
        }
    }
    return std::string();
}

std::string_view billingName(Billing billing) {
    return billing == Billing::Api ? "api" : "subscription";
}

bool limitedFor(const Registry* registry, std::string_view friendName, std::string_view account) {
    if (registry == nullptr) {
        return true;
    }
    const Friend* f = registry->friendNamed(friendName);
    if (f == nullptr || f->limit) {
        return true;
    }
    if (account.empty()) {
        return false;
    }
    // An account the files do not name reads limited: the safe direction.
    const Account* a = registry->account(account);
    return a == nullptr || a->billing == Billing::Api;
}

std::vector<std::string> changes(const Registry& before, const Registry& after) {
    std::vector<std::string> out;
    for (const Account& a : after.accounts) {
        const Account* was = before.account(a.name);
        if (was == nullptr) {
            out.push_back("account " + a.name + " set up (" + a.provider +
                          (a.billing == Billing::Api ? ", billed at API rate)" : ")"));
            continue;
        }
        if (was->label != a.label) {
            out.push_back("account " + a.name + "'s display label changed");
        }
        if (was->email != a.email) {
            out.push_back("account " + a.name + "'s email changed");
        }
        if (was->signInOnUse != a.signInOnUse) {
            out.push_back("account " + a.name + "'s sign_in_on_use is now " + (a.signInOnUse ? "true" : "false"));
        }
        if (was->provider != a.provider) {
            out.push_back("account " + a.name + "'s provider " + was->provider + " -> " + a.provider);
        }
        if (was->billing != a.billing) {
            out.push_back("account " + a.name + "'s billing " + std::string(billingName(was->billing)) + " -> " +
                          std::string(billingName(a.billing)));
        }
    }
    for (const Account& a : before.accounts) {
        if (after.account(a.name) == nullptr) {
            out.push_back("account " + a.name + " removed");
        }
    }
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& x : v) {
            s += (s.empty() ? "" : ", ") + x;
        }
        return "[" + s + "]";
    };
    for (const Friend& f : after.friends) {
        const Friend* was = before.friendNamed(f.name);
        if (was == nullptr) {
            std::vector<std::string> names;
            for (const Instance& i : f.instances) {
                names.push_back(i.name + " (" + i.machine + " " + i.dir + ")");
            }
            out.push_back("friend " + f.name + " set up, accounts " + list(f.accounts) + ", default " +
                          (f.preferred.empty() ? std::string("none") : f.preferred) + ", instances " + list(names) +
                          ", limit " + (f.limit ? "true" : "false"));
            continue;
        }
        if (was->limit != f.limit) {
            out.push_back("friend " + f.name + "'s limit " + (was->limit ? "true" : "false") + " -> " +
                          (f.limit ? "true" : "false"));
        }
        if (was->accounts != f.accounts) {
            out.push_back("friend " + f.name + "'s accounts " + list(was->accounts) + " -> " + list(f.accounts));
        }
        if (was->preferred != f.preferred) {
            out.push_back("friend " + f.name + "'s default " + (was->preferred.empty() ? std::string("none") :
                          was->preferred) + " -> " + (f.preferred.empty() ? std::string("none") : f.preferred));
        }
        for (const Instance& i : f.instances) {
            const Instance* old = was->instance(i.name);
            if (old == nullptr) {
                out.push_back("friend " + f.name + "'s instance " + i.name + " set up, on " + i.machine + " in " +
                              i.dir);
            } else if (*old != i) {
                out.push_back("friend " + f.name + "'s instance " + i.name + " moved: " + old->machine + " " +
                              old->dir + " -> " + i.machine + " " + i.dir);
            }
        }
        for (const Instance& i : was->instances) {
            if (f.instance(i.name) == nullptr) {
                out.push_back("friend " + f.name + "'s instance " + i.name + " removed");
            }
        }
    }
    for (const Friend& f : before.friends) {
        if (after.friendNamed(f.name) == nullptr) {
            out.push_back("friend " + f.name + " removed");
        }
    }
    if (before.keepsOut != after.keepsOut) {
        out.push_back("limit_keeps_out " + list(before.keepsOut) + " -> " + list(after.keepsOut));
    }
    return out;
}

} // namespace nexus::accounts
