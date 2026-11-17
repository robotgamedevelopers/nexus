// Tests of the Tailscale shim's C API (src/tailscale/nts.h), linked as nexus links it (cmake/tailscale.cmake): the
// c-archive on macOS and Linux, the DLL through its MSVC import library on Windows, with the Windows 10 manifest. It
// links the shim's test build, which adds the hooks of nts_test_hooks.h and is otherwise nexus's.
//
// Unit groups: no OS (no file, socket or thread of their own), no clock, no network.
//   version      the versions the shim reports are the pins: tailscale.com v1.104.7 and go1.27.4
//   licences_unit  nts_licences returns the embedded UTF-8 licence bundle, static and NUL-terminated, with every
//                  section carrying non-empty licence text
//   names        nts_state_name and nts_reason_name for every state and reason, and "unknown" for others
//   handles      every call that takes a handle refuses one never issued, a null out-pointer, and a bad port, with a
//                reason; the reason is cut to the caller's buffer
//   start_checks every refusal a start makes before it touches the state directory: no parameters, no directory or
//                hostname, a control URL that is not https (or has a user, path, query or fragment, or names
//                Tailscale's hosted service), an auth key with the hosted service, a tsnet sign-in variable set
//                (named, never its value)
//   panics_unit  a panic injected into each export is stopped there, with a reason
// Functional groups: the real filesystem, in a directory of the test's own, and (where said) nodes on sockets on this
// machine only. Offline nodes (nts_test_start_offline) use a control server on 127.0.0.1 that refuses at once; the
// test control server (nts_test_control_start) is Tailscale's in-tree one, run in this process on 127.0.0.1 with its
// own DERP relay and STUN server. No test reaches another machine, Tailscale's hosted service, or a real tailnet.
//   licences     every module@version and licence file in src/tailscale/NOTICES.md is in nts_licences, 1-to-1
//   state_dir    the state directory and its log file created owner-only (0700 and 0600; on Windows a protected ACL
//                granting only this user), an existing one tightened
//   refusals     the refusals that need the filesystem: a null, empty or relative path, a file in the way, an
//                unwritable parent (skipped, saying so, when this user can write to it anyway), a directory that
//                recorded another control server, a layer 0 directory (the hosted service's) given to another
//   refuses_inherited_key  with TS_AUTHKEY set from the process's start (ctest's ENVIRONMENT), a start refuses
//   lock         a second start, or a prepare, on a state directory a node of this process holds is refused, before
//                anything in it is opened; closing the node frees it (offline nodes)
//   lock_two_process  the same across processes: a child (hold_lock) holds the directory (offline nodes)
//   panics       a panic in each export with a node running: the panic's text goes to its log (offline nodes)
//   log_cap      a node's log past 1 MiB moves to nexus-tailscale.log.1 and a new one begins (offline nodes)
//   quiet        a child (quiet_child) runs nodes, the library loggers, and the panics group with its stdout and
//                stderr in files: both stay empty (offline nodes)
//   unreachable  an offline node says starting, with reason control-unreachable and the logged error as its text
//   certificate  a node whose control server's certificate no one trusts fails, with reason certificate
//   watch        the state callback: called at once and on changes, never after nts_close returns, and nts_close from
//                inside it (offline nodes)
//   threads      the API from many threads at once on one node, closed under them (offline nodes)
//   sign_in      the test control server: needs-login with the URL, approval, ready; the URL only in needs-login and
//                never in the shim's log; the record of the control server
//   tailnet      two nodes on the test control server: peers and tags, who-is (and "unknown"), TCP and UDP both ways
//                by DNS name, the handle rules (double close, use after close, wrong kind, busy, a close from another
//                thread ending a blocked read, a node's close closing its handles)
//   restart      a signed-in node closed and started again on its state directory is ready with no sign-in
//   expiry       a node's key expired by control: expired, a new login keeping its identity, ready again
//   control_down control stopped and started: control-unreachable while down, connections kept, ready again
//   auth_key     a control server that requires an auth key: a wrong one fails (auth-key), the right one is ready
//                with no URL, and an ephemeral node
// By hand only, never from ctest or CI: start_needs_login <dir> starts a node on a scratch state directory, which
// contacts Tailscale's hosted control server, waits until it asks to sign in, prints the state, and closes it. It never
// prints the sign-in URL.
// The functional groups wait only on conditions (a state, a peer seen, a file written), each bounded generously; none
// sleeps for a fixed time to let something happen.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "nts.h"
#include "nts_test_hooks.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

// This program's own path (main), for the groups that run it as a child.
std::string self;

// How long a functional group waits for a condition before it fails: generous, since a loaded CI machine is slow, and
// never spent unless something is wrong.
constexpr auto kPatience = std::chrono::seconds(60);

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A shell command that runs this program with args. cmd.exe strips the first and last quote of a command that holds
// more than two, so on Windows the whole command is quoted once more.
std::string command(const std::string& args) {
    std::string text = "\"" + self + "\" " + args;
#if defined(_WIN32)
    text = "\"" + text + "\"";
#endif
    return text;
}

nts_start_params params(const std::string& dir, const char* hostname, const char* control = nullptr,
                        const char* key = nullptr) {
    nts_start_params p{};
    p.dir = dir.c_str();
    p.hostname = hostname;
    p.control_url = control;
    p.auth_key = key;
    return p;
}

// A start of an offline node (nts_test_start_offline); its handle, or 0 with the reason in why.
int startOffline(const fs::path& dir, std::string& why, const char* hostname = "nexus-test") {
    char err[512];
    std::string path = dir.string();
    nts_start_params p = params(path, hostname);
    int node = nts_test_start_offline(&p, err, sizeof err);
    why = err;
    return node;
}

bool closeNode(int node) {
    char err[512];
    int rc = nts_close(node, err, sizeof err);
    if (rc != 0) {
        std::fprintf(stderr, "nts_close: %s\n", err);
    }
    return rc == 0;
}

// A fresh directory of this case's own under the test's working directory.
fs::path fresh(const char* name) {
    fs::path dir = fs::current_path() / name;
    std::error_code ec;
#if !defined(_WIN32)
    // A parent an earlier run left unwritable is made writable again first, so that it can be removed.
    if (fs::exists(dir / "locked", ec)) {
        fs::permissions(dir / "locked", fs::perms::owner_all, ec);
    }
#endif
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string prepare(const fs::path& dir) {
    char err[512];
    std::memset(err, 'x', sizeof err);
    int rc = nts_prepare(dir.string().c_str(), err, sizeof err);
    if (rc != 0) {
        return err[0] == 0 ? std::string("(failed with no reason)") : std::string(err);
    }
    CHECK(err[0] == 0);
    return {};
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

#if defined(_WIN32)
// The current user's SID, from this process's token.
std::string userSid() {
    HANDLE token = nullptr;
    std::string sid;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        sid.resize(size);
        if (!GetTokenInformation(token, TokenUser, sid.data(), size, &size)) {
            sid.clear();
        }
        CloseHandle(token);
    }
    return sid;
}

PSID sidOf(std::string& tokenUser) {
    return reinterpret_cast<TOKEN_USER*>(tokenUser.data())->User.Sid;
}

// Whether every entry in path's ACL grants to this user alone, and for a directory that the ACL is protected (nothing
// inherited from above; a file inside inherits the directory's).
bool ownerOnlyAcl(const fs::path& path) {
    bool directory = fs::is_directory(path);
    std::string user = userSid();
    if (user.empty()) {
        return false;
    }
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = dacl != nullptr && dacl->AceCount > 0;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
        (directory && (control & SE_DACL_PROTECTED) == 0)) {
        ok = false;
    }
    for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) {
            ok = false;
            break;
        }
        auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            ok = false;
            break;
        }
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (!EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), sidOf(user))) {
            ok = false;
        }
    }
    LocalFree(descriptor);
    return ok;
}

// Gives path a protected ACL granting this user only access (inherited by what is inside when inherit is set).
bool setAcl(const fs::path& path, DWORD access, bool inherit) {
    std::string user = userSid();
    if (user.empty()) {
        return false;
    }
    EXPLICIT_ACCESS_W entry{};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = SET_ACCESS;
    entry.grfInheritance = inherit ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sidOf(user));
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &entry, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring name = path.wstring();
    DWORD rc = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                     nullptr, acl, nullptr);
    LocalFree(acl);
    return rc == ERROR_SUCCESS;
}
#else
fs::perms modeOf(const fs::path& path) {
    return fs::status(path).permissions() & fs::perms::mask;
}
#endif

// Makes dir a directory this user cannot add to.
bool lock(const fs::path& dir) {
#if defined(_WIN32)
    return setAcl(dir, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
#else
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec, ec);
    return !ec;
#endif
}

void unlock(const fs::path& dir) {
#if defined(_WIN32)
    CHECK(setAcl(dir, GENERIC_ALL, true));
#else
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, ec);
    CHECK(!ec);
#endif
}

// ---- unit groups ----

void version() {
    const char* v = nts_version();
    CHECK(v != nullptr);
    if (v == nullptr) {
        return;
    }
    std::string text = v;
    CHECK(text == "tailscale.com v1.104.7 go1.27.4");
    // Static: the same pointer every call.
    CHECK(nts_version() == v);
}

struct LicenceFile {
    std::string name;
    std::string body;
};

struct LicenceSection {
    std::string header;
    std::vector<LicenceFile> files;
};

std::string trimSpace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(start, end - start);
}

bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0) {
            return false;
        }
        size_t need = 0;
        unsigned int code = 0;
        unsigned int minCode = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
            code = c & 0x1F;
            minCode = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
            code = c & 0x0F;
            minCode = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            code = c & 0x07;
            minCode = 0x10000;
        } else {
            return false;
        }
        if (i + need >= s.size()) {
            return false;
        }
        for (size_t j = 1; j <= need; ++j) {
            unsigned char cc = static_cast<unsigned char>(s[i + j]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (cc & 0x3F);
        }
        if (code < minCode || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += need + 1;
    }
    return true;
}

std::vector<LicenceSection> parseLicenceBundle(const std::string& bundle) {
    const std::string rule(80, '=');
    const std::string marker = "\n" + rule + "\n";
    std::vector<LicenceSection> sections;
    size_t pos = bundle.find(marker);
    while (pos != std::string::npos) {
        size_t headerStart = pos + marker.size();
        size_t headerEnd = bundle.find("\n" + rule + "\n", headerStart);
        if (headerEnd == std::string::npos) {
            break;
        }
        size_t bodyStart = headerEnd + rule.size() + 2;
        size_t nextPos = bundle.find(marker, bodyStart);
        std::string body =
            nextPos == std::string::npos ? bundle.substr(bodyStart) : bundle.substr(bodyStart, nextPos - bodyStart);
        LicenceSection sec;
        sec.header = bundle.substr(headerStart, headerEnd - headerStart);
        std::string withLeadingNewline = "\n" + body;
        size_t fpos = withLeadingNewline.find("\n--- ");
        while (fpos != std::string::npos) {
            size_t lineEnd = withLeadingNewline.find(" ---\n", fpos + 5);
            if (lineEnd == std::string::npos) {
                break;
            }
            std::string fname = withLeadingNewline.substr(fpos + 5, lineEnd - (fpos + 5));
            size_t contentStart = lineEnd + 5;
            size_t nextFile = withLeadingNewline.find("\n--- ", contentStart);
            std::string fbody = nextFile == std::string::npos
                                    ? withLeadingNewline.substr(contentStart)
                                    : withLeadingNewline.substr(contentStart, nextFile - contentStart);
            sec.files.push_back(LicenceFile{fname, trimSpace(fbody)});
            fpos = nextFile;
        }
        sections.push_back(std::move(sec));
        pos = nextPos;
    }
    return sections;
}

void licencesUnit() {
    size_t len = 0;
    const char* raw = nts_licences(&len);
    CHECK(raw != nullptr);
    if (raw == nullptr) {
        return;
    }
    CHECK(nts_licences(nullptr) == raw);
    CHECK(len > 100000);
    CHECK(std::strlen(raw) == len);
    std::string bundle(raw, len);
    CHECK(!has(bundle, "\r"));
    CHECK(validUtf8(bundle));
    std::vector<LicenceSection> sections = parseLicenceBundle(bundle);
    CHECK(sections.size() >= 38);
    if (sections.empty()) {
        return;
    }
    CHECK(sections[0].header == "Go standard library and runtime go1.27.4 (BSD-3-Clause)");
    CHECK(sections[0].files.size() == 1 && sections[0].files[0].name == "LICENSE" &&
          has(sections[0].files[0].body, "Redistribution and use in source and binary forms"));
    bool sawTailscale = false;
    std::string prevModule;
    for (size_t i = 1; i < sections.size(); ++i) {
        const LicenceSection& s = sections[i];
        size_t at = s.header.find('@');
        CHECK(at != std::string::npos && at > 0);
        if (at != std::string::npos) {
            std::string mod = s.header.substr(0, at);
            CHECK(prevModule.empty() || prevModule < mod);
            prevModule = mod;
        }
        if (s.header == "tailscale.com@v1.104.7 (BSD-3-Clause)") {
            sawTailscale = true;
        }
        CHECK(!s.files.empty());
        for (const LicenceFile& f : s.files) {
            CHECK(!f.name.empty());
            CHECK(f.body.size() >= 100);
        }
    }
    CHECK(sawTailscale);
}

void names() {
    const char* states[] = {"not-started", "starting", "needs-login", "running",
                            "ready",       "expired",  "stopped",     "failed"};
    const int stateCodes[] = {NTS_STATE_NOT_STARTED, NTS_STATE_STARTING, NTS_STATE_NEEDS_LOGIN, NTS_STATE_RUNNING,
                              NTS_STATE_READY,       NTS_STATE_EXPIRED,  NTS_STATE_STOPPED,     NTS_STATE_FAILED};
    for (int i = 0; i < 8; ++i) {
        CHECK(std::string(nts_state_name(stateCodes[i])) == states[i]);
    }
    const char* reasons[] = {"none",    "auth-key",           "tags",             "refused", "certificate",
                             "logging", "control-unreachable", "machine-approval", "closed",  "control-stopped"};
    const int reasonCodes[] = {NTS_REASON_NONE,
                               NTS_REASON_AUTH_KEY,
                               NTS_REASON_TAGS,
                               NTS_REASON_REFUSED,
                               NTS_REASON_CERTIFICATE,
                               NTS_REASON_LOGGING,
                               NTS_REASON_CONTROL_UNREACHABLE,
                               NTS_REASON_MACHINE_APPROVAL,
                               NTS_REASON_CLOSED,
                               NTS_REASON_CONTROL_STOPPED};
    for (int i = 0; i < 10; ++i) {
        CHECK(std::string(nts_reason_name(reasonCodes[i])) == reasons[i]);
    }
    CHECK(std::string(nts_state_name(-1)) == "unknown" && std::string(nts_state_name(8)) == "unknown");
    CHECK(std::string(nts_reason_name(-1)) == "unknown" && std::string(nts_reason_name(10)) == "unknown");
    // Static: the same pointer every call.
    CHECK(nts_state_name(NTS_STATE_READY) == nts_state_name(NTS_STATE_READY));
}

const char* const kNotIssued = "is not a handle this process was given";

// Every call that takes a handle, with handles no call has issued (this group issues none).
void handles() {
    char err[512];
    char text[64];
    unsigned char buf[16] = {};
    nts_status status{};
    nts_peer peer{};
    nts_whois who{};
    for (int h : {0, -1, 7, 123456}) {
        CHECK(nts_close(h, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_status_get(h, &status, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_status_wait(h, 0, 0, &status, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_login_url(h, text, sizeof text, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_watch(h, nullptr, nullptr, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_login(h, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_peers(h, &peer, 1, 0, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_peer_find(h, "a.example", &peer, 0, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_whois_addr(h, "192.0.2.1:1", &who, 0, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_listen_tcp(h, 80, err, sizeof err) == 0 && has(err, kNotIssued));
        CHECK(nts_accept(h, 0, err, sizeof err) == NTS_ERROR && has(err, kNotIssued));
        CHECK(nts_dial_tcp(h, "a:1", 0, err, sizeof err) == 0 && has(err, kNotIssued));
        CHECK(nts_dial_udp(h, "a:1", 0, err, sizeof err) == 0 && has(err, kNotIssued));
        CHECK(nts_listen_udp(h, 80, err, sizeof err) == 0 && has(err, kNotIssued));
        CHECK(nts_read(h, buf, sizeof buf, 0, err, sizeof err) == NTS_ERROR && has(err, kNotIssued));
        CHECK(nts_write(h, buf, sizeof buf, 0, err, sizeof err) == NTS_ERROR && has(err, kNotIssued));
        CHECK(nts_recv_from(h, buf, sizeof buf, 0, text, sizeof text, err, sizeof err) == NTS_ERROR &&
              has(err, kNotIssued));
        CHECK(nts_send_to(h, buf, sizeof buf, "a:1", 0, err, sizeof err) == NTS_ERROR && has(err, kNotIssued));
        CHECK(nts_local_addr(h, text, sizeof text, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_remote_addr(h, text, sizeof text, err, sizeof err) == -1 && has(err, kNotIssued));
        CHECK(nts_handle_close(h, err, sizeof err) == -1 && has(err, kNotIssued));
    }
    // Null out-pointers and bad ports are refused before the handle is looked at.
    CHECK(nts_status_get(1, nullptr, err, sizeof err) == -1 && has(err, "null"));
    CHECK(nts_status_wait(1, 0, 0, nullptr, err, sizeof err) == -1 && has(err, "null"));
    CHECK(nts_peers(1, nullptr, 3, 0, err, sizeof err) == -1 && has(err, "null"));
    CHECK(nts_peers(1, &peer, -1, 0, err, sizeof err) == -1);
    CHECK(nts_peer_find(1, nullptr, &peer, 0, err, sizeof err) == -1);
    CHECK(nts_whois_addr(1, "not an address", &who, 0, err, sizeof err) == -1 && has(err, "not ip:port"));
    for (int port : {0, -1, 65536}) {
        CHECK(nts_listen_tcp(1, port, err, sizeof err) == 0 && has(err, "is not 1..65535"));
        CHECK(nts_listen_udp(1, port, err, sizeof err) == 0 && has(err, "is not 1..65535"));
    }
    // The reason is cut to the caller's buffer and always ends; a null buffer or a size of 0 is allowed.
    char tiny[8];
    std::memset(tiny, 'x', sizeof tiny);
    CHECK(nts_close(1, tiny, sizeof tiny) == -1 && std::strlen(tiny) == 7);
    CHECK(nts_close(1, nullptr, 0) == -1);
    tiny[0] = 'x';
    CHECK(nts_close(1, tiny, 0) == -1 && tiny[0] == 'x');
}

// Refusals before a start touches its state directory: the directory named is never created.
void startChecks() {
    char err[512];
    const std::string dir = (fs::current_path() / "start_checks_never_made").string();
    auto refused = [&](const nts_start_params& p, const char* part) {
        int node = nts_start(&p, err, sizeof err);
        bool ok = node == 0 && has(err, part);
        if (!ok) {
            std::fprintf(stderr, "start_checks: expected a refusal with \"%s\", got %d \"%s\"\n", part, node, err);
        }
        return ok;
    };
    CHECK(nts_start(nullptr, err, sizeof err) == 0 && has(err, "no parameters"));
    CHECK(refused(params("", "nexus-test"), "state directory"));
    CHECK(refused(params(dir, ""), "hostname"));
    CHECK(refused(params(dir, nullptr), "hostname"));
    CHECK(refused(params(dir, "nexus-test", "http://control.example"), "not https"));
    CHECK(refused(params(dir, "nexus-test", "ftp://control.example"), "not https"));
    CHECK(refused(params(dir, "nexus-test", "control.example"), "not https"));
    CHECK(refused(params(dir, "nexus-test", "https://user:a-password@control.example"), "user name or password"));
    CHECK(!has(err, "a-password"));
    CHECK(refused(params(dir, "nexus-test", "https://control.example/some/path"), "more than a host"));
    CHECK(refused(params(dir, "nexus-test", "https://control.example?q=1"), "more than a host"));
    CHECK(refused(params(dir, "nexus-test", "https://controlplane.tailscale.com"), "hosted service"));
    CHECK(refused(params(dir, "nexus-test", "https://login.tailscale.com"), "hosted service"));
    // The hosted service in other spellings is still the hosted service, and refused with an auth key or without.
    for (const char* hosted : {"https://controlplane.tailscale.com.", "https://CONTROLPLANE.tailscale.com:443",
                               "https://controlplane.tailscale.com.:443/"}) {
        CHECK(refused(params(dir, "nexus-test", hosted), "hosted service"));
        CHECK(refused(params(dir, "nexus-test", hosted, "tskey-a-value-that-must-not-be-shown"), "hosted service"));
        CHECK(!has(err, "must-not-be-shown"));
    }
    // "\xef\xbd\x83..." is fullwidth "controlplane"; "\xe3\x80\x82" an ideographic full stop. IDNA maps both to the
    // hosted service's name, so neither is mapped: the host must be given in ASCII.
    CHECK(refused(params(dir, "nexus-test",
                         "https://\xef\xbd\x83\xef\xbd\x8f\xef\xbd\x8e\xef\xbd\x94\xef\xbd\x92\xef\xbd\x8f\xef\xbd\x8c"
                         "\xef\xbd\x90\xef\xbd\x8c\xef\xbd\x81\xef\xbd\x8e\xef\xbd\x85.tailscale.com",
                         "tskey-a-value-that-must-not-be-shown"),
                  "not ASCII"));
    CHECK(refused(params(dir, "nexus-test", "https://controlplane.tailscale\xe3\x80\x82" "com"), "not ASCII"));
    CHECK(refused(params(dir, "nexus-test", "https://xn--a.example"), "IDNA"));
    for (const char* port :
         {"https://control.example:", "https://control.example:0", "https://control.example:99999"}) {
        CHECK(refused(params(dir, "nexus-test", port), "port is not a number from 1 to 65535"));
    }
    CHECK(refused(params(dir, "nexus-test", "https://[fe80::1%25en0]"), "zone"));
    CHECK(refused(params(dir, "nexus-test", nullptr, "tskey-a-value-that-must-not-be-shown"), "auth key"));
    nts_start_params p = params(dir, "nexus-test", nullptr, "tskey-a-value-that-must-not-be-shown");
    CHECK(nts_start(&p, err, sizeof err) == 0 && !has(err, "must-not-be-shown"));
    // A start refuses before anything else while a variable of the environment policy's refused list
    // (src/tailscale/environment.txt) is set, naming it (never its value).
    for (const char* name : {"TS_AUTHKEY", "TS_AUTH_KEY", "TS_CLIENT_SECRET", "TS_CLIENT_ID", "TS_ID_TOKEN",
                             "TS_AUDIENCE", "TS_CONTROL_URL", "TSNET_FORCE_LOGIN", "SSLKEYLOGFILE", "SSL_CERT_FILE",
                             "SSL_CERT_DIR", "GODEBUG", "TS_DEBUG_MAP", "TS_LOG_TARGET", "TAILSCALE_USE_WIP_CODE"}) {
        setEnv(name, "a-value-that-must-not-be-shown");
        p = params(dir, "nexus-test");
        CHECK(nts_start(&p, err, sizeof err) == 0);
        CHECK(has(err, name) && has(err, "only from nts_start's parameters") &&
              !has(err, "a-value-that-must-not-be-shown"));
        setEnv(name, nullptr);
    }
    CHECK(!fs::exists(dir));
}

const char* const kPanicReason = "stopped by an internal error (a Go panic)";

// A panic injected into each export is stopped there (no node runs: the panic's text is dropped).
void panicsUnit() {
    char err[512];
    char text[64];
    unsigned char buf[4] = {};
    nts_status status{};
    nts_peer peer{};
    nts_whois who{};
    nts_start_params p = params("/never", "nexus-test");
    struct Case {
        const char* name;
        std::function<bool()> call;
    };
    const Case cases[] = {
        {"nts_prepare", [&] { return nts_prepare("/never", err, sizeof err) == -1; }},
        {"nts_start", [&] { return nts_start(&p, err, sizeof err) == 0; }},
        {"nts_test_start_offline", [&] { return nts_test_start_offline(&p, err, sizeof err) == 0; }},
        {"nts_close", [&] { return nts_close(1, err, sizeof err) == -1; }},
        {"nts_status_get", [&] { return nts_status_get(1, &status, err, sizeof err) == -1; }},
        {"nts_status_wait", [&] { return nts_status_wait(1, 0, 0, &status, err, sizeof err) == -1; }},
        {"nts_login_url", [&] { return nts_login_url(1, text, sizeof text, err, sizeof err) == -1; }},
        {"nts_watch", [&] { return nts_watch(1, nullptr, nullptr, err, sizeof err) == -1; }},
        {"nts_login", [&] { return nts_login(1, err, sizeof err) == -1; }},
        {"nts_peers", [&] { return nts_peers(1, &peer, 1, 0, err, sizeof err) == -1; }},
        {"nts_peer_find", [&] { return nts_peer_find(1, "a", &peer, 0, err, sizeof err) == -1; }},
        {"nts_whois_addr", [&] { return nts_whois_addr(1, "192.0.2.1:1", &who, 0, err, sizeof err) == -1; }},
        {"nts_listen_tcp", [&] { return nts_listen_tcp(1, 80, err, sizeof err) == 0; }},
        {"nts_accept", [&] { return nts_accept(1, 0, err, sizeof err) == NTS_ERROR; }},
        {"nts_dial_tcp", [&] { return nts_dial_tcp(1, "a:1", 0, err, sizeof err) == 0; }},
        {"nts_dial_udp", [&] { return nts_dial_udp(1, "a:1", 0, err, sizeof err) == 0; }},
        {"nts_listen_udp", [&] { return nts_listen_udp(1, 80, err, sizeof err) == 0; }},
        {"nts_read", [&] { return nts_read(1, buf, sizeof buf, 0, err, sizeof err) == NTS_ERROR; }},
        {"nts_write", [&] { return nts_write(1, buf, sizeof buf, 0, err, sizeof err) == NTS_ERROR; }},
        {"nts_recv_from",
         [&] { return nts_recv_from(1, buf, sizeof buf, 0, text, sizeof text, err, sizeof err) == NTS_ERROR; }},
        {"nts_send_to", [&] { return nts_send_to(1, buf, sizeof buf, "a:1", 0, err, sizeof err) == NTS_ERROR; }},
        {"nts_local_addr", [&] { return nts_local_addr(1, text, sizeof text, err, sizeof err) == -1; }},
        {"nts_remote_addr", [&] { return nts_remote_addr(1, text, sizeof text, err, sizeof err) == -1; }},
        {"nts_handle_close", [&] { return nts_handle_close(1, err, sizeof err) == -1; }},
    };
    for (const Case& c : cases) {
        nts_test_inject_panic(c.name);
        bool failed = c.call();
        CHECK(failed && has(err, kPanicReason));
        if (!failed || !has(err, kPanicReason)) {
            std::fprintf(stderr, "panics_unit: %s: %s\n", c.name, err);
        }
    }
    nts_test_inject_panic("nts_version");
    CHECK(nts_version() != nullptr && std::string(nts_version()) == "tailscale.com v1.104.7 go1.27.4");
    size_t licenceLen = 0;
    nts_test_inject_panic("nts_licences");
    const char* lic = nts_licences(&licenceLen);
    CHECK(lic != nullptr && licenceLen > 0 && std::strlen(lic) == licenceLen);
    nts_test_inject_panic("nts_state_name");
    CHECK(std::string(nts_state_name(NTS_STATE_READY)) == "unknown");
    nts_test_inject_panic("nts_reason_name");
    CHECK(std::string(nts_reason_name(NTS_REASON_NONE)) == "unknown");
    nts_test_inject_panic(nullptr);
    CHECK(std::string(nts_state_name(NTS_STATE_READY)) == "ready");
}

// ---- functional groups: the filesystem, offline nodes ----

// Every row of src/tailscale/NOTICES.md is carried in nts_licences(), 1-to-1 in order, with each named licence file's
// text non-empty.
void licences() {
    std::string notices = readFile(NEXUS_TAILSCALE_NOTICES_MD);
    CHECK(!notices.empty());
    struct NoticeRow {
        std::string header;
        std::vector<std::string> files;
    };
    std::vector<NoticeRow> rows;
    size_t lineStart = 0;
    while (lineStart < notices.size()) {
        size_t lineEnd = notices.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = notices.size();
        }
        std::string line = trimSpace(notices.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
        if (line.size() < 2 || line[0] != '|' || has(line, "| Module |") || has(line, "|---|")) {
            continue;
        }
        std::vector<std::string> cells;
        size_t pos = 1;
        while (pos < line.size()) {
            size_t bar = line.find('|', pos);
            if (bar == std::string::npos) {
                break;
            }
            cells.push_back(trimSpace(line.substr(pos, bar - pos)));
            pos = bar + 1;
        }
        CHECK(cells.size() == 5);
        if (cells.size() != 5) {
            continue;
        }
        NoticeRow row;
        row.header = cells[0] + "@" + cells[1] + " (" + cells[2] + ")";
        size_t fpos = 0;
        while (fpos < cells[3].size()) {
            size_t comma = cells[3].find(',', fpos);
            std::string part =
                comma == std::string::npos ? cells[3].substr(fpos) : cells[3].substr(fpos, comma - fpos);
            std::string clean = trimSpace(part);
            if (!clean.empty()) {
                row.files.push_back(clean);
            }
            if (comma == std::string::npos) {
                break;
            }
            fpos = comma + 1;
        }
        CHECK(!row.files.empty());
        rows.push_back(std::move(row));
    }
    CHECK(rows.size() >= 37);

    size_t len = 0;
    const char* raw = nts_licences(&len);
    CHECK(raw != nullptr);
    if (raw == nullptr) {
        return;
    }
    std::vector<LicenceSection> sections = parseLicenceBundle(std::string(raw, len));
    CHECK(sections.size() == rows.size() + 1);
    if (sections.size() != rows.size() + 1) {
        std::fprintf(stderr, "licences: NOTICES.md lists %zu modules, nts_licences holds %zu sections\n", rows.size(),
                     sections.size());
        return;
    }
    CHECK(sections[0].header == "Go standard library and runtime go1.27.4 (BSD-3-Clause)");
    for (size_t i = 0; i < rows.size(); ++i) {
        const NoticeRow& want = rows[i];
        const LicenceSection& got = sections[i + 1];
        CHECK(got.header == want.header);
        if (got.header != want.header) {
            std::fprintf(stderr, "licences: row %zu: want \"%s\", got \"%s\"\n", i, want.header.c_str(),
                         got.header.c_str());
            continue;
        }
        CHECK(got.files.size() == want.files.size());
        for (size_t j = 0; j < want.files.size() && j < got.files.size(); ++j) {
            CHECK(got.files[j].name == want.files[j]);
            CHECK(got.files[j].body.size() >= 100);
        }
    }
}

void stateDir() {
    fs::path work = fresh("state_dir");
    // Created with its missing parents, owner-only, with its log file.
    fs::path dir = work / "home" / "tailscale";
    CHECK(prepare(dir).empty());
    CHECK(fs::is_directory(dir));
    CHECK(fs::is_regular_file(dir / "nexus-tailscale.log"));
#if defined(_WIN32)
    CHECK(ownerOnlyAcl(dir));
    CHECK(ownerOnlyAcl(work / "home"));
    CHECK(ownerOnlyAcl(dir / "nexus-tailscale.log"));
#else
    CHECK(modeOf(dir) == fs::perms::owner_all);
    CHECK(modeOf(work / "home") == fs::perms::owner_all);
    CHECK(modeOf(dir / "nexus-tailscale.log") == (fs::perms::owner_read | fs::perms::owner_write));
#endif
    // Prepared again: still so, and the log file kept (appended to, never truncated).
    {
        std::ofstream(dir / "nexus-tailscale.log", std::ios::app | std::ios::binary) << "kept\n";
    }
    CHECK(prepare(dir).empty());
    CHECK(fs::file_size(dir / "nexus-tailscale.log") == 5);

    // An existing directory and log file open to others are made owner-only.
    fs::path open = work / "open";
    fs::create_directories(open);
    {
        std::ofstream(open / "nexus-tailscale.log") << "";
    }
#if defined(_WIN32)
    CHECK(!ownerOnlyAcl(open));
    CHECK(prepare(open).empty());
    CHECK(ownerOnlyAcl(open));
    CHECK(ownerOnlyAcl(open / "nexus-tailscale.log"));
#else
    fs::permissions(open, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec);
    fs::permissions(open / "nexus-tailscale.log", fs::perms::owner_read | fs::perms::owner_write |
                                                      fs::perms::group_read | fs::perms::others_read);
    CHECK(prepare(open).empty());
    CHECK(modeOf(open) == fs::perms::owner_all);
    CHECK(modeOf(open / "nexus-tailscale.log") == (fs::perms::owner_read | fs::perms::owner_write));
#endif
}

void refusals() {
    fs::path work = fresh("refusals");
    char err[512];
    std::string why;

    // No path, an empty one, a relative one.
    CHECK(nts_prepare(nullptr, err, sizeof err) == -1 && has(err, "no state directory"));
    CHECK(nts_prepare("", err, sizeof err) == -1 && has(err, "no state directory"));
    CHECK(nts_prepare("relative/state", err, sizeof err) == -1 && has(err, "not an absolute path"));
    CHECK(!fs::exists(fs::current_path() / "relative"));

    // A file where the directory or a parent of it would be.
    fs::path file = work / "file";
    {
        std::ofstream(file) << "in the way";
    }
    why = prepare(file);
    CHECK(has(why, "is not a directory"));
    why = prepare(file / "state");
    CHECK(has(why, "cannot create the state directory"));

    // A parent this user cannot write to, once the test has seen that this user really cannot (root, or an
    // administrator's backup privilege, writes anyway): otherwise skipped, saying so.
    fs::path locked = work / "locked";
    fs::create_directories(locked);
    CHECK(lock(locked));
    std::error_code probeError;
    if (fs::create_directory(locked / "probe", probeError)) {
        fs::remove(locked / "probe", probeError);
        std::printf("refusals: skipped the unwritable-parent checks: this user can write to %s although it is locked "
                    "(root, or an administrator?)\n",
                    locked.string().c_str());
    } else {
        why = prepare(locked / "state");
        CHECK(has(why, "cannot create the state directory") && has(why, "fix the permissions of its parent"));
        CHECK(!fs::exists(locked / "state"));
        CHECK(startOffline(locked / "state", why) == 0 && has(why, "cannot create the state directory"));
    }
    unlock(locked);

    // A directory that recorded another control server is refused, before tsnet starts on it.
    fs::path other = work / "other_control";
    fs::create_directories(other);
    {
        std::ofstream(other / "nexus-tailscale.control") << "https://control.example\n";
    }
    CHECK(startOffline(other, why) == 0 && has(why, "the control server https://control.example's"));
    CHECK(!fs::exists(other / "tailscaled.state"));
    // A node's state and no record (the record removed outside nexus): refused, since whose state it is cannot be
    // known, and nothing is written there.
    fs::path unrecorded = work / "unrecorded";
    fs::create_directories(unrecorded);
    {
        std::ofstream(unrecorded / "tailscaled.state") << "{}";
    }
    CHECK(startOffline(unrecorded, why) == 0 && has(why, "no record of its control server"));
    CHECK(readFile(unrecorded / "tailscaled.state") == "{}");
    CHECK(!fs::exists(unrecorded / "nexus-tailscale.control"));
    // An offline start records its control server, and a second start there agrees with it.
    fs::path recorded = work / "recorded";
    int node = startOffline(recorded, why);
    CHECK(node > 0);
    CHECK(node <= 0 || closeNode(node));
    CHECK(readFile(recorded / "nexus-tailscale.control") == "http://127.0.0.1:9\n");
#if !defined(_WIN32)
    CHECK(modeOf(recorded / "nexus-tailscale.control") == (fs::perms::owner_read | fs::perms::owner_write));
#endif
    node = startOffline(recorded, why);
    CHECK(node > 0);
    CHECK(node <= 0 || closeNode(node));
}

// Run with TS_AUTHKEY in the environment from the process's start (ctest sets it), as a key inherited from a shell would
// be, rather than set by the program: a start refuses, naming it and never its value.
void refusesInheritedKey() {
    fs::path work = fresh("inherited");
    char err[512];
    CHECK(std::getenv("TS_AUTHKEY") != nullptr);
    std::string dir = (work / "state").string();
    nts_start_params p = params(dir, "nexus-test");
    CHECK(nts_start(&p, err, sizeof err) == 0);
    CHECK(has(err, "TS_AUTHKEY") && !has(err, "a-value-that-must-not-be-shown"));
    CHECK(!fs::exists(work / "state"));
}

// A state directory is one node's: a second start or a prepare on it is refused while a node of this process holds
// it, however the path is spelled, and allowed again once the node closes.
void lockInProcess() {
    fs::path work = fresh("lock");
    fs::path dir = work / "state";
    std::string why;
    int node = startOffline(dir, why);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "lock: %s\n", why.c_str());
        return;
    }
    CHECK(fs::is_regular_file(dir / "nexus-tailscale.lock"));
    CHECK(startOffline(dir, why) == 0 && has(why, "in use by a node this process started"));
    CHECK(startOffline(dir.string() + "/", why) == 0 && has(why, "in use by a node this process started"));
    CHECK(startOffline(dir / "sub" / "..", why) == 0 && has(why, "in use by a node this process started"));
    CHECK(!fs::exists(dir / "sub"));
    why = prepare(dir);
    CHECK(has(why, "in use by a node this process started"));
    // A different directory is not held.
    int other = startOffline(work / "other", why);
    CHECK(other > 0);
    CHECK(other <= 0 || closeNode(other));
    CHECK(closeNode(node));
    // Free again.
    CHECK(prepare(dir).empty());
    node = startOffline(dir, why);
    CHECK(node > 0);
    CHECK(node <= 0 || closeNode(node));
}

// The child of lock_two_process: starts an offline node on dir, writes "held" (or why not) to marker, and closes the
// node once its stdin ends.
int holdLock(const char* dir, const char* marker) {
    std::string why;
    int node = startOffline(dir, why);
    fs::path temporary = std::string(marker) + ".partial";
    {
        std::ofstream(temporary, std::ios::binary) << (node > 0 ? std::string("held") : "failed: " + why);
    }
    std::error_code ec;
    fs::rename(temporary, marker, ec);
    if (node <= 0) {
        return 1;
    }
    while (std::fgetc(stdin) != EOF) {
    }
    return closeNode(node) ? 0 : 1;
}

void lockTwoProcess() {
    fs::path work = fresh("lock_two_process");
    fs::path dir = work / "state";
    fs::path marker = work / "held";
    std::string args = "hold_lock \"" + dir.string() + "\" \"" + marker.string() + "\"";
#if defined(_WIN32)
    FILE* child = _popen(command(args).c_str(), "w");
#else
    FILE* child = popen(command(args).c_str(), "w");
#endif
    CHECK(child != nullptr);
    if (child == nullptr) {
        return;
    }
    auto deadline = Clock::now() + kPatience;
    while (!fs::exists(marker) && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::string held = readFile(marker);
    CHECK(held == "held");
    if (held == "held") {
        std::string why;
        CHECK(startOffline(dir, why) == 0 && has(why, "in use by another process's node"));
        why = prepare(dir);
        CHECK(has(why, "in use by another process's node"));
    } else {
        std::fprintf(stderr, "lock_two_process: the child did not hold the directory: %s\n", held.c_str());
    }
#if defined(_WIN32)
    int rc = _pclose(child);
#else
    int rc = pclose(child);
#endif
    CHECK(rc == 0);
    // The child's node is closed and its process gone: the directory is free.
    std::string why;
    int node = startOffline(dir, why);
    CHECK(node > 0);
    CHECK(node <= 0 || closeNode(node));
}

// A panic with a node running: the call fails, the node keeps running, and the panic's text goes to its log. A panic
// after the directory is held and its log opened gives both back.
void panics() {
    fs::path work = fresh("panics");
    fs::path dir = work / "state";
    std::string path = dir.string();
    char err[512];
    nts_start_params p = params(path, "nexus-test");
    nts_test_inject_panic("nts_test_start_offline after open");
    CHECK(nts_test_start_offline(&p, err, sizeof err) == 0 && has(err, kPanicReason));
    nts_test_inject_panic(nullptr);
    std::string why;
    int node = startOffline(dir, why);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "panics: %s\n", why.c_str());
        return;
    }
    nts_status status{};
    nts_test_inject_panic("nts_status_get");
    CHECK(nts_status_get(node, &status, err, sizeof err) == -1 && has(err, kPanicReason));
    size_t licenceLen = 0;
    nts_test_inject_panic("nts_licences");
    CHECK(nts_licences(&licenceLen) != nullptr);
    nts_test_inject_panic(nullptr);
    CHECK(nts_status_get(node, &status, err, sizeof err) == 0);
    CHECK(closeNode(node));
    std::string logText = readFile(dir / "nexus-tailscale.log");
    CHECK(has(logText, "nts test panic injected at nts_status_get"));
    CHECK(has(logText, "nts test panic injected at nts_licences"));
}

// A node's log is capped: past 1 MiB (logCap in src/tailscale/shim/output.go) nexus-tailscale.log moves to
// nexus-tailscale.log.1, replacing an older one, and a new one begins, so the two never hold more than 2 MiB.
void logCap() {
    fs::path work = fresh("log_cap");
    fs::path dir = work / "state";
    std::string why;
    int node = startOffline(dir, why);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "log_cap: %s\n", why.c_str());
        return;
    }
    const std::uintmax_t cap = 1 << 20;
    std::string line(1000, 'x');
    // About 3 KB a call, 3 MB in all: two rotations at least.
    for (int i = 0; i < 1000; ++i) {
        nts_test_log_libraries(line.c_str());
    }
    CHECK(closeNode(node));
    fs::path log = dir / "nexus-tailscale.log";
    fs::path old = dir / "nexus-tailscale.log.1";
    CHECK(fs::is_regular_file(log) && fs::file_size(log) <= cap);
    CHECK(fs::is_regular_file(old) && fs::file_size(old) <= cap && fs::file_size(old) > cap / 2);
    CHECK(!fs::exists(dir / "nexus-tailscale.log.2"));
#if !defined(_WIN32)
    CHECK(modeOf(old) == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK(modeOf(log) == (fs::perms::owner_read | fs::perms::owner_write));
#endif
}

void panicsAll() {
    panicsUnit();
    panics();
}

// The child of quiet, run with its stdout and stderr in files: a node's start and close, the library loggers before,
// during and after it, and both panics groups. It prints nothing unless a check fails.
void quietChild() {
    fs::path work = fresh("quiet_child");
    fs::path dir = work / "state";
    nts_test_log_libraries("before any node");
    std::string why;
    int node = startOffline(dir, why);
    CHECK(node > 0);
    if (node > 0) {
        nts_test_log_libraries("while a node runs");
        CHECK(closeNode(node));
    } else {
        std::fprintf(stderr, "quiet_child: %s\n", why.c_str());
    }
    nts_test_log_libraries("after the node closed");
    std::string log = readFile(dir / "nexus-tailscale.log");
    CHECK(has(log, "go log: while a node runs"));
    CHECK(has(log, "gvisor warning: while a node runs"));
    CHECK(has(log, "gvisor info: while a node runs"));
    CHECK(!has(log, "before any node") && !has(log, "after the node closed"));
    panicsAll();
}

void quiet() {
    fs::path out = fs::current_path() / "quiet_stdout.txt";
    fs::path errFile = fs::current_path() / "quiet_stderr.txt";
    std::error_code ec;
    fs::remove(out, ec);
    fs::remove(errFile, ec);
    int rc = std::system(command("quiet_child >quiet_stdout.txt 2>quiet_stderr.txt").c_str());
    std::string printed = readFile(out);
    std::string errors = readFile(errFile);
    CHECK(rc == 0);
    CHECK(fs::exists(out) && printed.empty());
    CHECK(fs::exists(errFile) && errors.empty());
    if (!printed.empty() || !errors.empty()) {
        std::fprintf(stderr, "quiet_child's stdout:\n%s\nquiet_child's stderr:\n%s\n", printed.c_str(),
                     errors.c_str());
    }
}

// Waits until the node's state satisfies want, at most kPatience, on its generations (nts_status_wait): never a
// fixed sleep. The last state seen is left in st.
bool waitFor(int node, const std::function<bool(const nts_status&)>& want, nts_status& st) {
    char err[512];
    if (nts_status_get(node, &st, err, sizeof err) != 0) {
        std::fprintf(stderr, "waitFor: %s\n", err);
        return false;
    }
    auto deadline = Clock::now() + kPatience;
    while (!want(st)) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            std::fprintf(stderr, "waitFor: timed out in %s (%s): %s\n", nts_state_name(st.state),
                         nts_reason_name(st.reason), st.reason_text);
            return false;
        }
        if (nts_status_wait(node, st.generation, static_cast<int>(left), &st, err, sizeof err) == -1) {
            std::fprintf(stderr, "waitFor: %s\n", err);
            return false;
        }
    }
    return true;
}

bool waitState(int node, int state, nts_status& st) {
    return waitFor(node, [state](const nts_status& s) { return s.state == state; }, st);
}

// Waits on a condition outside the node's state (a peer seen, a callback run), looking every 20 ms, at most
// kPatience.
bool eventually(const std::function<bool()>& condition) {
    auto deadline = Clock::now() + kPatience;
    while (!condition()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

// An offline node: starting, or needing a sign-in that has no URL yet, with the control server's refusal (seen only in
// tsnet's log) as its reason.
void unreachable() {
    fs::path work = fresh("unreachable");
    std::string why;
    int node = startOffline(work / "state", why);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "unreachable: %s\n", why.c_str());
        return;
    }
    nts_status st{};
    CHECK(waitFor(node, [](const nts_status& s) { return s.reason == NTS_REASON_CONTROL_UNREACHABLE; }, st));
    CHECK(st.state == NTS_STATE_STARTING || st.state == NTS_STATE_NEEDS_LOGIN);
    CHECK(st.reason_text[0] != 0);
    char url[256];
    char err[512];
    CHECK(nts_login_url(node, url, sizeof url, err, sizeof err) == -1 && has(err, "no sign-in URL now"));
    CHECK(closeNode(node));
    char e[512];
    CHECK(nts_status_get(node, &st, e, sizeof e) == -1 && has(e, "is closed"));
    CHECK(nts_close(node, e, sizeof e) == -1 && has(e, "is closed"));
}

// The test control server, started for one group and stopped when it ends.
struct Control {
    std::string url;
    bool ok = false;
    explicit Control(bool requireAuth, const char* authKey = nullptr) {
        char buffer[256];
        char err[512];
        ok = nts_test_control_start(requireAuth ? 1 : 0, authKey, buffer, sizeof buffer, err, sizeof err) == 0;
        if (!ok) {
            std::fprintf(stderr, "nts_test_control_start: %s\n", err);
            return;
        }
        url = buffer;
    }
    ~Control() {
        char err[512];
        if (ok && nts_test_control_stop(err, sizeof err) != 0) {
            std::fprintf(stderr, "nts_test_control_stop: %s\n", err);
            ++failures;
        }
    }
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
};

int start(const fs::path& dir, const char* hostname, const std::string& control, const char* key = nullptr,
          bool ephemeral = false) {
    char err[512];
    std::string path = dir.string();
    nts_start_params p = params(path, hostname, control.c_str(), key);
    p.ephemeral = ephemeral ? 1 : 0;
    int node = nts_start(&p, err, sizeof err);
    if (node == 0) {
        std::fprintf(stderr, "nts_start: %s\n", err);
    }
    return node;
}

// The sign-in URL, once the node asks for one; empty if it never does.
std::string loginUrl(int node) {
    nts_status st{};
    if (!waitFor(node, [](const nts_status& s) { return s.state == NTS_STATE_NEEDS_LOGIN && s.reason == 0; }, st)) {
        return {};
    }
    char url[1024];
    char err[512];
    std::string found;
    // needs-login comes with the URL, or just before it.
    eventually([&] {
        if (nts_login_url(node, url, sizeof url, err, sizeof err) == 0) {
            found = url;
            return true;
        }
        return false;
    });
    return found;
}

// A node started on the test control server and signed in: ready, or 0.
int signedIn(const fs::path& dir, const char* hostname, const Control& control) {
    int node = start(dir, hostname, control.url);
    if (node == 0) {
        return 0;
    }
    std::string url = loginUrl(node);
    char err[512];
    if (url.empty() || nts_test_control_approve(url.c_str(), err, sizeof err) != 0) {
        std::fprintf(stderr, "signedIn: no sign-in to approve: %s\n", url.empty() ? "no URL" : err);
        return 0;
    }
    nts_status st{};
    if (!waitState(node, NTS_STATE_READY, st)) {
        return 0;
    }
    return node;
}

void signIn() {
    fs::path work = fresh("sign_in");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    fs::path dir = work / "state";
    int node = start(dir, "nexus-sign-in", control.url);
    CHECK(node > 0);
    if (node <= 0) {
        return;
    }
    std::string url = loginUrl(node);
    CHECK(!url.empty() && has(url, control.url));
    // Never cut: a buffer one byte short is refused.
    char tooShort[512];
    char err[512];
    if (url.size() < sizeof tooShort) {
        CHECK(nts_login_url(node, tooShort, url.size(), err, sizeof err) == -1 && has(err, "never cut"));
    }
    nts_status st{};
    CHECK(nts_status_get(node, &st, err, sizeof err) == 0 && st.state == NTS_STATE_NEEDS_LOGIN);
    CHECK(nts_test_control_approve(url.c_str(), err, sizeof err) == 0);
    CHECK(waitState(node, NTS_STATE_READY, st));
    CHECK(st.reason == NTS_REASON_NONE);
    // The URL is gone once the node is past needs-login.
    char after[1024];
    CHECK(nts_login_url(node, after, sizeof after, err, sizeof err) == -1 && has(err, "no sign-in URL now"));
    CHECK(closeNode(node));
    CHECK(nts_status_get(node, &st, err, sizeof err) == -1);
    // The shim's log never holds the URL; the directory recorded its control server.
    CHECK(!url.empty() && !has(readFile(dir / "nexus-tailscale.log"), url));
    CHECK(readFile(dir / "nexus-tailscale.control") == control.url + "\n");
}

void* const kNoUser = nullptr;

struct WatchCount {
    std::atomic<int> calls{0};
    std::atomic<unsigned long long> last{0};
    std::atomic<int> target{0};
    std::atomic<int> closed{-2};
};

void countCalls(void* user, int, unsigned long long generation) {
    auto* w = static_cast<WatchCount*>(user);
    w->last = generation;
    ++w->calls;
}

void closeFromCallback(void* user, int node, unsigned long long) {
    auto* w = static_cast<WatchCount*>(user);
    ++w->calls;
    if (w->target.exchange(0) == node) {
        char err[512];
        w->closed = nts_close(node, err, sizeof err);
    }
}

void watch() {
    fs::path work = fresh("watch");
    std::string why;
    int node = startOffline(work / "a", why);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "watch: %s\n", why.c_str());
        return;
    }
    char err[512];
    WatchCount count;
    CHECK(nts_watch(node, countCalls, &count, err, sizeof err) == 0);
    // Called once at once, then on each change: the offline node becomes control-unreachable.
    CHECK(eventually([&] { return count.calls.load() >= 1; }));
    nts_status st{};
    CHECK(waitFor(node, [](const nts_status& s) { return s.reason == NTS_REASON_CONTROL_UNREACHABLE; }, st));
    CHECK(eventually([&] { return count.last.load() >= st.generation; }));
    CHECK(closeNode(node));
    int calls = count.calls.load();
    // nts_close has returned: no callback runs now, whatever else the process does.
    std::string otherWhy;
    int other = startOffline(work / "b", otherWhy);
    CHECK(other > 0);
    CHECK(other <= 0 || waitFor(other, [](const nts_status& s) { return s.reason != NTS_REASON_NONE; }, st));
    CHECK(count.calls.load() == calls);
    // Removing the callback: none is called.
    WatchCount none;
    CHECK(other <= 0 || nts_watch(other, countCalls, &none, err, sizeof err) == 0);
    CHECK(eventually([&] { return none.calls.load() >= 1; }));
    CHECK(other <= 0 || nts_watch(other, nullptr, kNoUser, err, sizeof err) == 0);
    CHECK(other <= 0 || closeNode(other));
    // nts_close from inside the callback.
    int third = startOffline(work / "c", why);
    CHECK(third > 0);
    if (third > 0) {
        WatchCount closing;
        closing.target = third;
        CHECK(nts_watch(third, closeFromCallback, &closing, err, sizeof err) == 0);
        CHECK(eventually([&] { return closing.closed.load() != -2; }));
        CHECK(closing.closed.load() == 0);
        CHECK(nts_status_get(third, &st, err, sizeof err) == -1 && has(err, "is closed"));
    }
}

// The API from many threads at once on one node, which another thread closes under them: every call answers (a
// result, or an error with a reason), and nothing crashes.
void threads() {
    fs::path work = fresh("threads");
    std::string why;
    int node = startOffline(work / "state", why);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "threads: %s\n", why.c_str());
        return;
    }
    std::atomic<bool> stop{false};
    std::atomic<int> answered{0};
    std::atomic<int> silent{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t] {
            while (!stop.load()) {
                char err[512] = "unset";
                nts_status st{};
                nts_peer peer{};
                int rc = 0;
                switch (t % 4) {
                case 0:
                    rc = nts_status_get(node, &st, err, sizeof err);
                    break;
                case 1:
                    rc = nts_status_wait(node, st.generation, 5, &st, err, sizeof err);
                    break;
                case 2:
                    rc = nts_peers(node, &peer, 1, 50, err, sizeof err);
                    break;
                default:
                    rc = nts_listen_tcp(node, 9000 + t, err, sizeof err);
                    if (rc > 0) {
                        nts_handle_close(rc, err, sizeof err);
                    }
                    break;
                }
                if (rc < 0 && rc != NTS_TIMEOUT && err[0] == 0) {
                    ++silent;
                }
                ++answered;
            }
        });
    }
    CHECK(eventually([&] { return answered.load() > 200; }));
    CHECK(closeNode(node));
    // Calls after the close answer too, with an error.
    int before = answered.load();
    CHECK(eventually([&] { return answered.load() > before + 100; }));
    stop = true;
    for (std::thread& w : workers) {
        w.join();
    }
    CHECK(silent.load() == 0);

    // Closing a node from another thread ends an unbounded nts_status_wait (-1) with an error, whether after is the
    // node's current generation or ahead of any generation the node ever reaches.
    auto checkWaitClosed = [&](const char* sub, unsigned long long delta) {
        int waiting = startOffline(work / sub, why);
        CHECK(waiting > 0);
        if (waiting <= 0) {
            return;
        }
        nts_status st{};
        CHECK(waitFor(waiting, [](const nts_status& s) { return s.reason == NTS_REASON_CONTROL_UNREACHABLE; }, st));
        std::atomic<bool> entered{false};
        std::atomic<bool> finished{false};
        std::atomic<int> waitRc{-99};
        char waitErr[512] = {};
        nts_status waitOut{};
        std::thread waiter([&] {
            nts_status cur = st;
            entered = true;
            for (;;) {
                int r = nts_status_wait(waiting, cur.generation + delta, -1, &waitOut, waitErr, sizeof waitErr);
                if (r == 0 && waitOut.reason == NTS_REASON_CLOSED) {
                    waitRc = 0;
                    break;
                }
                if (r != 0) {
                    waitRc = r;
                    break;
                }
                cur = waitOut;
            }
            finished = true;
        });
        CHECK(eventually([&] { return entered.load(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(closeNode(waiting));
        auto until = Clock::now() + std::chrono::seconds(2);
        while (!finished.load() && Clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(finished.load());
        if (!finished.load()) {
            std::fprintf(stderr, "threads (%s): nts_status_wait(after=+%llu, -1) hung after nts_close\n", sub, delta);
            std::fflush(stderr);
            std::_Exit(1);
        }
        waiter.join();
        CHECK(waitRc.load() == -1 && waitErr[0] != 0);
        char afterCloseErr[512] = {};
        CHECK(nts_status_wait(waiting, 0, -1, &waitOut, afterCloseErr, sizeof afterCloseErr) == -1 &&
              has(afterCloseErr, "is closed"));
    };
    checkWaitClosed("wait_current", 0);
    checkWaitClosed("wait_ahead", 5);
}

// ---- functional groups: the test control server ----

// Whether node sees a peer named dnsName (its DNS name) online.
bool seesPeer(int node, const std::string& dnsName, nts_peer& out) {
    char err[512];
    return nts_peer_find(node, dnsName.c_str(), &out, 1000, err, sizeof err) == 1 && out.online != 0;
}

std::string dnsOf(const char* hostname) {
    return std::string(hostname) + ".tailnet.example";
}

// A byte round trip on two connected handles: a writes, b reads it all.
bool carries(int a, int b, const std::string& text) {
    char err[512];
    if (nts_write(a, text.data(), text.size(), 10000, err, sizeof err) != static_cast<long long>(text.size())) {
        std::fprintf(stderr, "carries: write: %s\n", err);
        return false;
    }
    std::string got;
    auto deadline = Clock::now() + kPatience;
    while (got.size() < text.size() && Clock::now() < deadline) {
        char buf[256];
        long long n = nts_read(b, buf, sizeof buf, 1000, err, sizeof err);
        if (n > 0) {
            got.append(buf, static_cast<size_t>(n));
        } else if (n != NTS_TIMEOUT) {
            std::fprintf(stderr, "carries: read: %lld %s\n", n, err);
            return false;
        }
    }
    return got == text;
}

void tailnet() {
    fs::path work = fresh("tailnet");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    int a = signedIn(work / "a", "nexus-a", control);
    int b = signedIn(work / "b", "nexus-b", control);
    CHECK(a > 0 && b > 0);
    if (a <= 0 || b <= 0) {
        return;
    }
    char err[512];
    // Peers, matched by DNS name (letter case and a trailing dot ignored), with tags an admin gives.
    nts_peer peer{};
    CHECK(eventually([&] { return seesPeer(a, dnsOf("nexus-b"), peer); }));
    CHECK(std::string(peer.host_name) == "nexus-b");
    CHECK(std::string(peer.addresses).rfind("100.", 0) == 0);
    CHECK(nts_peer_find(a, "NEXUS-B.tailnet.example.", &peer, 1000, err, sizeof err) == 1);
    CHECK(nts_peer_find(a, "nexus-nobody.tailnet.example", &peer, 1000, err, sizeof err) == 0);
    CHECK(nts_peers(a, nullptr, 0, 1000, err, sizeof err) == 1);
    nts_peer list[4] = {};
    CHECK(nts_peers(a, list, 4, 1000, err, sizeof err) == 1 && std::string(list[0].dns_name) == dnsOf("nexus-b"));
    CHECK(nts_test_control_set_tags("nexus-b", "tag:server tag:test", err, sizeof err) == 0);
    CHECK(eventually([&] { return seesPeer(a, dnsOf("nexus-b"), peer) && has(peer.tags, "tag:server"); }));
    CHECK(std::string(peer.tags) == "tag:server tag:test");

    // TCP by DNS name, both ways, and who-is for the remote address.
    int listener = nts_listen_tcp(b, 7000, err, sizeof err);
    CHECK(listener > 0);
    int dialed = nts_dial_tcp(a, (dnsOf("nexus-b") + ":7000").c_str(), 30000, err, sizeof err);
    CHECK(dialed > 0);
    if (dialed <= 0) {
        std::fprintf(stderr, "tailnet: dial: %s\n", err);
    }
    int accepted = nts_accept(listener, 30000, err, sizeof err);
    CHECK(accepted > 0);
    if (dialed > 0 && accepted > 0) {
        CHECK(carries(dialed, accepted, "hello over the tailnet"));
        CHECK(carries(accepted, dialed, "and back"));
        char remote[128];
        CHECK(nts_remote_addr(accepted, remote, sizeof remote, err, sizeof err) == 0);
        nts_whois who{};
        CHECK(nts_whois_addr(b, remote, &who, 5000, err, sizeof err) == 1);
        CHECK(std::string(who.node_name) == dnsOf("nexus-a"));
        CHECK(who.login[0] != 0);
        char local[128];
        CHECK(nts_local_addr(dialed, local, sizeof local, err, sizeof err) == 0 && std::string(local) == remote);
        // Who-is for an address that is no node: "unknown", an answer and not an error.
        CHECK(nts_whois_addr(b, "100.64.99.99:1", &who, 5000, err, sizeof err) == 0);

        // A read with nothing to read times out; the timeout 0 does not wait.
        char buf[16];
        CHECK(nts_read(accepted, buf, sizeof buf, 0, err, sizeof err) == NTS_TIMEOUT);
        // One read at a time: a second while one waits is refused as busy; a close from another thread ends the
        // waiting one with an error.
        std::atomic<long long> blocked{1};
        std::thread reader([&] {
            char e[512];
            char data[16];
            // Until this read is the one that waits (the main thread's probes below may hold the read side a moment).
            do {
                blocked = nts_read(accepted, data, sizeof data, -1, e, sizeof e);
            } while (blocked.load() == NTS_ERROR && has(e, "busy"));
        });
        CHECK(eventually([&] {
            char e[512];
            long long n = nts_read(accepted, buf, sizeof buf, 0, e, sizeof e);
            return n == NTS_ERROR && has(e, "busy");
        }));
        CHECK(nts_handle_close(accepted, err, sizeof err) == 0);
        reader.join();
        CHECK(blocked.load() == NTS_ERROR);
        // Double close and use after close are errors.
        CHECK(nts_handle_close(accepted, err, sizeof err) == -1 && has(err, "is closed"));
        CHECK(nts_read(accepted, buf, sizeof buf, 0, err, sizeof err) == NTS_ERROR && has(err, "is closed"));
        // The far end sees the stream end.
        long long end = 1;
        CHECK(eventually([&] {
            end = nts_read(dialed, buf, sizeof buf, 100, err, sizeof err);
            return end != NTS_TIMEOUT;
        }));
        CHECK(end == 0 || end == NTS_ERROR);
    }
    // Wrong kinds.
    char buf[16];
    CHECK(nts_read(listener, buf, sizeof buf, 0, err, sizeof err) == NTS_ERROR && has(err, "not a connection"));
    CHECK(nts_accept(a, 0, err, sizeof err) == NTS_ERROR && has(err, "node's handle"));
    CHECK(nts_handle_close(a, err, sizeof err) == -1 && has(err, "nts_close"));
    CHECK(nts_accept(listener, 0, err, sizeof err) == NTS_TIMEOUT);

    // UDP: a socket on b, a connected one from a by DNS name, and replies by address and by name.
    int sockB = nts_listen_udp(b, 7001, err, sizeof err);
    CHECK(sockB > 0);
    int udpA = nts_dial_udp(a, (dnsOf("nexus-b") + ":7001").c_str(), 30000, err, sizeof err);
    CHECK(udpA > 0);
    if (sockB > 0 && udpA > 0) {
        std::string from;
        std::string got;
        // UDP may drop the first datagrams while the path forms: send until one arrives.
        CHECK(eventually([&] {
            char e[512];
            nts_write(udpA, "ping", 4, 1000, e, sizeof e);
            char data[64];
            char sender[128];
            long long n = nts_recv_from(sockB, data, sizeof data, 200, sender, sizeof sender, e, sizeof e);
            if (n > 0) {
                got.assign(data, static_cast<size_t>(n));
                from = sender;
                return true;
            }
            return false;
        }));
        CHECK(got == "ping");
        CHECK(nts_send_to(sockB, "pong", 4, from.c_str(), 5000, err, sizeof err) == 4);
        char data[64];
        long long n = nts_read(udpA, data, sizeof data, 30000, err, sizeof err);
        CHECK(n == 4 && std::string(data, 4) == "pong");
        int sockA = nts_listen_udp(a, 7002, err, sizeof err);
        CHECK(sockA > 0);
        if (sockA > 0) {
            CHECK(eventually([&] {
                char e[512];
                nts_send_to(sockB, "by name", 7, (dnsOf("nexus-a") + ":7002").c_str(), 1000, e, sizeof e);
                char d[64];
                char sender[128];
                long long m = nts_recv_from(sockA, d, sizeof d, 200, sender, sizeof sender, e, sizeof e);
                return m == 7 && std::string(d, 7) == "by name";
            }));
        }
        CHECK(nts_send_to(sockB, "x", 1, "nexus-nobody:1", 1000, err, sizeof err) == NTS_ERROR &&
              has(err, "no peer"));

        // UDP truncation: the count is the datagram's whole length, and the buffer holds what fit.
        CHECK(eventually([&] {
            char e[512];
            nts_write(udpA, "0123456789", 10, 1000, e, sizeof e);
            char head[4] = {};
            char sender[128];
            long long m = nts_recv_from(sockB, head, sizeof head, 200, sender, sizeof sender, e, sizeof e);
            return m == 10 && std::memcmp(head, "0123", 4) == 0;
        }));
        CHECK(eventually([&] {
            char e[512];
            nts_send_to(sockB, "abcdefgh", 8, from.c_str(), 1000, e, sizeof e);
            char head[3] = {};
            long long m = nts_read(udpA, head, sizeof head, 200, e, sizeof e);
            return m == 8 && std::memcmp(head, "abc", 3) == 0;
        }));
        // An absurd size is refused by validation, before the buffer is touched.
        char one[1];
        CHECK(nts_recv_from(sockB, one, SIZE_MAX, 0, nullptr, 0, err, sizeof err) == NTS_ERROR &&
              has(err, "above the most"));
        CHECK(nts_send_to(sockB, one, SIZE_MAX, from.c_str(), 0, err, sizeof err) == NTS_ERROR &&
              has(err, "above the most"));
        CHECK(nts_read(udpA, one, SIZE_MAX, 0, err, sizeof err) == NTS_ERROR && has(err, "above the most"));
        CHECK(nts_write(udpA, one, SIZE_MAX, 0, err, sizeof err) == NTS_ERROR && has(err, "above the most"));
    }

    // Dials and datagrams go only to the tailnet's peers: loopback (a trap listening there), a private address, an
    // address in the tailnet's range that no peer has, a public name and the host's own name are each refused with
    // the reason, and nothing was sent (no call into tsnet that sends, nothing at the trap).
    int trap = nts_test_host_trap(err, sizeof err);
    CHECK(trap > 0);
    long long calls = nts_test_net_calls();
    long long trapped = nts_test_host_trap_count();
    const std::string port = std::to_string(trap);
    const std::string offTailnet[] = {"127.0.0.1:" + port, "[::1]:" + port, "localhost:" + port, "10.1.2.3:" + port,
                                      "100.64.99.99:" + port, "example.com:443"};
    for (const std::string& to : offTailnet) {
        CHECK(nts_dial_tcp(a, to.c_str(), 1000, err, sizeof err) == 0 && has(err, "no peer"));
        CHECK(nts_dial_udp(a, to.c_str(), 1000, err, sizeof err) == 0 && has(err, "no peer"));
        CHECK(sockB <= 0 || (nts_send_to(sockB, "x", 1, to.c_str(), 1000, err, sizeof err) == NTS_ERROR &&
                             has(err, "no peer")));
    }
    CHECK(nts_test_net_calls() == calls);
    CHECK(nts_test_host_trap_count() == trapped);
    // Even if a peer were in target()'s snapshot and left the node's map just before the dial, the post-target dial
    // goes only through netstack and never falls back to the host's sockets.
    CHECK(nts_test_dial_after_target(a, "tcp", ("127.0.0.1:" + port).c_str(), 1000, err, sizeof err) == 0 &&
          has(err, "no longer a peer"));
    CHECK(nts_test_dial_after_target(a, "udp", ("127.0.0.1:" + port).c_str(), 1000, err, sizeof err) == 0 &&
          has(err, "no longer a peer"));
    CHECK(nts_test_dial_after_target(a, "tcp", "100.64.99.99:7000", 1000, err, sizeof err) == 0 &&
          has(err, "no longer a peer"));
    CHECK(nts_test_host_trap_count() == trapped);

    // Timeout 0: a dial is refused (it must wait for the peer); peers are answered from the node's own state; an
    // accept takes a connection already waiting; a read returns data already arrived.
    CHECK(nts_dial_tcp(a, (dnsOf("nexus-b") + ":7000").c_str(), 0, err, sizeof err) == 0 &&
          has(err, "timeout of 0"));
    CHECK(nts_peers(a, nullptr, 0, 0, err, sizeof err) == 1);
    int second = nts_dial_tcp(a, (dnsOf("nexus-b") + ":7000").c_str(), 30000, err, sizeof err);
    CHECK(second > 0);
    int waiting = 0;
    CHECK(second <= 0 || eventually([&] {
        waiting = nts_accept(listener, 0, err, sizeof err);
        return waiting != NTS_TIMEOUT;
    }));
    CHECK(second <= 0 || waiting > 0);
    if (second > 0 && waiting > 0) {
        CHECK(nts_write(second, "abc", 3, 10000, err, sizeof err) == 3);
        long long got = NTS_TIMEOUT;
        CHECK(eventually([&] {
            char three[8];
            got = nts_read(waiting, three, sizeof three, 0, err, sizeof err);
            return got != NTS_TIMEOUT;
        }));
        CHECK(got == 3);
        char huge[1];
        CHECK(nts_read(waiting, huge, SIZE_MAX, 0, err, sizeof err) == NTS_ERROR && has(err, "above the most"));
        CHECK(nts_write(waiting, huge, SIZE_MAX, 0, err, sizeof err) == NTS_ERROR && has(err, "above the most"));
    }
    // A node's close closes its handles.
    CHECK(closeNode(a));
    CHECK(dialed <= 0 || (nts_write(dialed, "x", 1, 0, err, sizeof err) == NTS_ERROR && has(err, "is closed")));
    CHECK(udpA <= 0 || nts_handle_close(udpA, err, sizeof err) == -1);
    CHECK(closeNode(b));
    CHECK(nts_handle_close(listener, err, sizeof err) == -1 && has(err, "is closed"));
}

void restart() {
    fs::path work = fresh("restart");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    fs::path dir = work / "state";
    int node = signedIn(dir, "nexus-restart", control);
    CHECK(node > 0);
    if (node <= 0) {
        return;
    }
    CHECK(closeNode(node));
    node = start(dir, "nexus-restart", control.url);
    CHECK(node > 0);
    if (node <= 0) {
        return;
    }
    // Ready with no sign-in: never a sign-in URL on the way.
    char err[512];
    nts_status st{};
    bool askedToSignIn = false;
    CHECK(waitFor(node,
                  [&](const nts_status& s) {
                      char url[1024];
                      char e[512];
                      if (nts_login_url(node, url, sizeof url, e, sizeof e) == 0) {
                          askedToSignIn = true;
                      }
                      return s.state == NTS_STATE_READY;
                  },
                  st));
    CHECK(!askedToSignIn);
    CHECK(nts_status_get(node, &st, err, sizeof err) == 0 && st.state == NTS_STATE_READY);
    CHECK(closeNode(node));
}

void expiry() {
    fs::path work = fresh("expiry");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    int a = signedIn(work / "a", "nexus-expiring", control);
    int b = signedIn(work / "b", "nexus-watcher", control);
    CHECK(a > 0 && b > 0);
    if (a <= 0 || b <= 0) {
        return;
    }
    nts_peer before{};
    CHECK(eventually([&] { return seesPeer(b, dnsOf("nexus-expiring"), before); }));
    char err[512];
    CHECK(nts_test_control_expire("nexus-expiring", err, sizeof err) == 0);
    nts_status st{};
    CHECK(waitState(a, NTS_STATE_EXPIRED, st));
    CHECK(st.key_expiry > 0);
    char url[1024];
    CHECK(nts_login_url(a, url, sizeof url, err, sizeof err) == -1);
    // A new login: a new URL, approved, and ready again with the same identity.
    CHECK(nts_login(a, err, sizeof err) == 0);
    std::string again = loginUrl(a);
    CHECK(!again.empty());
    CHECK(nts_test_control_approve(again.c_str(), err, sizeof err) == 0);
    CHECK(waitState(a, NTS_STATE_READY, st));
    nts_peer after{};
    CHECK(eventually([&] {
        return seesPeer(b, dnsOf("nexus-expiring"), after) && std::string(after.addresses) == before.addresses;
    }));
    CHECK(std::string(after.dns_name) == before.dns_name);
    CHECK(closeNode(a));
    CHECK(closeNode(b));
}

void controlDown() {
    fs::path work = fresh("control_down");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    int a = signedIn(work / "a", "nexus-a", control);
    int b = signedIn(work / "b", "nexus-b", control);
    CHECK(a > 0 && b > 0);
    if (a <= 0 || b <= 0) {
        return;
    }
    char err[512];
    nts_peer peer{};
    CHECK(eventually([&] { return seesPeer(a, dnsOf("nexus-b"), peer); }));
    int listener = nts_listen_tcp(b, 7100, err, sizeof err);
    int dialed = nts_dial_tcp(a, (dnsOf("nexus-b") + ":7100").c_str(), 30000, err, sizeof err);
    int accepted = nts_accept(listener, 30000, err, sizeof err);
    CHECK(listener > 0 && dialed > 0 && accepted > 0);
    CHECK(nts_test_control_down(err, sizeof err) == 0);
    nts_status st{};
    CHECK(waitFor(a, [](const nts_status& s) { return s.reason == NTS_REASON_CONTROL_UNREACHABLE; }, st));
    CHECK(st.state == NTS_STATE_READY && st.control_down != 0);
    // The tailnet keeps working without control.
    CHECK(dialed > 0 && accepted > 0 && carries(dialed, accepted, "while control is down"));
    CHECK(nts_test_control_up(err, sizeof err) == 0);
    CHECK(waitFor(a, [](const nts_status& s) { return s.state == NTS_STATE_READY && s.reason == 0; }, st));
    CHECK(st.control_down == 0);
    CHECK(dialed > 0 && accepted > 0 && carries(accepted, dialed, "control is back"));
    CHECK(closeNode(a));
    CHECK(closeNode(b));
}

void authKey() {
    fs::path work = fresh("auth_key");
    Control control(false, "tskey-test-only-key");
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    nts_status st{};
    int wrong = start(work / "wrong", "nexus-wrong-key", control.url, "tskey-not-the-key");
    CHECK(wrong > 0);
    if (wrong > 0) {
        CHECK(waitState(wrong, NTS_STATE_FAILED, st));
        CHECK(st.reason == NTS_REASON_AUTH_KEY && has(st.reason_text, "invalid authkey"));
        CHECK(!has(st.reason_text, "tskey-not-the-key"));
        CHECK(closeNode(wrong));
    }
    int right = start(work / "right", "nexus-right-key", control.url, "tskey-test-only-key", true);
    CHECK(right > 0);
    if (right > 0) {
        bool askedToSignIn = false;
        CHECK(waitFor(right,
                      [&](const nts_status& s) {
                          char url[1024];
                          char e[512];
                          if (nts_login_url(right, url, sizeof url, e, sizeof e) == 0) {
                              askedToSignIn = true;
                          }
                          return s.state == NTS_STATE_READY;
                      },
                      st));
        CHECK(!askedToSignIn);
        CHECK(!has(readFile(work / "right" / "nexus-tailscale.log"), "tskey-test-only-key"));
        CHECK(closeNode(right));
    }
}

// Control's own words reach the host scrubbed: an error naming the node's auth key, another key and a URL, longer
// than a reason holds, gives the reason code it names (auth-key) and a text with none of them, cut to 256 bytes. The
// node's log holds none of them either.
void controlError() {
    fs::path work = fresh("control_error");
    fs::path dir = work / "state";
    char err[512];
    std::string path = dir.string();
    nts_start_params p = params(path, "nexus-control-error", nullptr, "nexus-own-secret-key");
    int node = nts_test_start_offline(&p, err, sizeof err);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "control_error: %s\n", err);
        return;
    }
    std::string text = "invalid authkey nexus-own-secret-key (also tskey-auth-other-123); see "
                       "https://control.example/admin/machines?key=nexus-own-secret-key for more; " +
                       std::string(600, 'x');
    CHECK(nts_test_inject_control_error(node, text.c_str(), err, sizeof err) == 0);
    nts_status st{};
    CHECK(waitState(node, NTS_STATE_FAILED, st));
    std::string reason = st.reason_text;
    CHECK(st.reason == NTS_REASON_AUTH_KEY && has(reason, "invalid authkey"));
    CHECK(!has(reason, "nexus-own-secret-key") && !has(reason, "tskey-") && !has(reason, "://"));
    CHECK(has(reason, "<a secret, withheld>") && has(reason, "<a URL, withheld>"));
    CHECK(reason.size() <= 256);
    CHECK(closeNode(node));
    std::string log = readFile(dir / "nexus-tailscale.log");
    CHECK(!has(log, "nexus-own-secret-key") && !has(log, "tskey-auth-other"));
}

// The sign-in left waiting long enough for tsnet to repeat its URL line in the log (every 5 s), in a child whose
// stdout and stderr are files: the log holds the placeholder and never the URL, no reason text ever names a URL, and
// the child prints nothing at all.
void longSignInChild() {
    fs::path work = fresh("long_sign_in_child");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    fs::path dir = work / "state";
    int node = start(dir, "nexus-long-sign-in", control.url);
    CHECK(node > 0);
    if (node <= 0) {
        return;
    }
    std::string url = loginUrl(node);
    CHECK(!url.empty());
    bool urlInReason = false;
    auto look = [&] {
        nts_status s{};
        char e[512];
        if (nts_status_get(node, &s, e, sizeof e) == 0 && (has(s.reason_text, "://") ||
                                                           (!url.empty() && has(s.reason_text, url)))) {
            urlInReason = true;
        }
    };
    // tsnet's line with the URL, twice: at least one 5 s repeat.
    CHECK(eventually([&] {
        look();
        std::string log = readFile(dir / "nexus-tailscale.log");
        size_t first = log.find("<a URL, withheld>");
        return first != std::string::npos && log.find("<a URL, withheld>", first + 1) != std::string::npos;
    }));
    char err[512];
    CHECK(!url.empty() && nts_test_control_approve(url.c_str(), err, sizeof err) == 0);
    nts_status st{};
    CHECK(waitFor(node,
                  [&](const nts_status& s) {
                      look();
                      return s.state == NTS_STATE_READY;
                  },
                  st));
    look();
    CHECK(!urlInReason);
    CHECK(closeNode(node));
    std::string log = readFile(dir / "nexus-tailscale.log");
    CHECK(!url.empty() && !has(log, url) && !has(log, "://"));
}

void longSignIn() {
    std::error_code ec;
    fs::remove("long_stdout.txt", ec);
    fs::remove("long_stderr.txt", ec);
    int rc = std::system(command("long_sign_in_child >long_stdout.txt 2>long_stderr.txt").c_str());
    std::string printed = readFile(fs::current_path() / "long_stdout.txt");
    std::string errors = readFile(fs::current_path() / "long_stderr.txt");
    CHECK(rc == 0);
    CHECK(printed.empty() && errors.empty());
    if (rc != 0 || !printed.empty() || !errors.empty()) {
        // The child's output is only what its failed checks said: never the URL, which it does not print.
        std::fprintf(stderr, "long_sign_in_child: rc %d, %zu bytes on stdout, %zu on stderr\n", rc, printed.size(),
                     errors.size());
    }
}

// Control blackholed (its connections cut, then its port held silently): the node reports control unreachable at
// once (the map poll closed), stays ready with the control-down flag, and the tailnet keeps working; with control
// back, the flag clears.
void controlBlackhole() {
    fs::path work = fresh("control_blackhole");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    int a = signedIn(work / "a", "nexus-a", control);
    int b = signedIn(work / "b", "nexus-b", control);
    CHECK(a > 0 && b > 0);
    if (a <= 0 || b <= 0) {
        return;
    }
    char err[512];
    nts_peer peer{};
    CHECK(eventually([&] { return seesPeer(a, dnsOf("nexus-b"), peer); }));
    int listener = nts_listen_tcp(b, 7200, err, sizeof err);
    int dialed = nts_dial_tcp(a, (dnsOf("nexus-b") + ":7200").c_str(), 30000, err, sizeof err);
    int accepted = nts_accept(listener, 30000, err, sizeof err);
    CHECK(listener > 0 && dialed > 0 && accepted > 0);
    auto before = Clock::now();
    CHECK(nts_test_control_blackhole(err, sizeof err) == 0);
    nts_status st{};
    CHECK(waitFor(a, [](const nts_status& s) { return s.control_down != 0; }, st));
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - before).count();
    // At once: the poll's close is seen at the shim's next look (250 ms), never after a keep-alive's minutes.
    CHECK(took < 10000);
    CHECK(st.state == NTS_STATE_READY && st.reason == NTS_REASON_CONTROL_UNREACHABLE);
    CHECK(dialed > 0 && accepted > 0 && carries(dialed, accepted, "while control is blackholed"));
    CHECK(nts_test_control_up(err, sizeof err) == 0);
    CHECK(waitFor(a, [](const nts_status& s) { return s.state == NTS_STATE_READY && s.control_down == 0; }, st));
    CHECK(st.reason == NTS_REASON_NONE);
    CHECK(closeNode(a));
    CHECK(closeNode(b));
}

struct Pair {
    std::atomic<int> entered{0};
    std::atomic<int> closed[2] = {{-2}, {-2}};
    int nodes[2] = {0, 0};
};

struct Side {
    Pair* pair;
    int me;
};

// Each node's callback closes the other node, both at once: neither waits on the other.
void closeTheOther(void* user, int, unsigned long long) {
    auto* side = static_cast<Side*>(user);
    Pair* pair = side->pair;
    if (pair->closed[side->me].load() != -2) {
        return;
    }
    ++pair->entered;
    auto until = Clock::now() + std::chrono::seconds(10);
    while (pair->entered.load() < 2 && Clock::now() < until) {
        std::this_thread::yield();
    }
    char err[512];
    pair->closed[side->me] = nts_close(pair->nodes[1 - side->me], err, sizeof err);
}

void closeEachOther() {
    fs::path work = fresh("close_each_other");
    std::string why;
    Pair pair;
    pair.nodes[0] = startOffline(work / "x", why, "nexus-x");
    pair.nodes[1] = startOffline(work / "y", why, "nexus-y");
    CHECK(pair.nodes[0] > 0 && pair.nodes[1] > 0);
    if (pair.nodes[0] <= 0 || pair.nodes[1] <= 0) {
        std::fprintf(stderr, "close_each_other: %s\n", why.c_str());
        return;
    }
    Side x{&pair, 0};
    Side y{&pair, 1};
    char err[512];
    CHECK(nts_watch(pair.nodes[0], closeTheOther, &x, err, sizeof err) == 0);
    CHECK(nts_watch(pair.nodes[1], closeTheOther, &y, err, sizeof err) == 0);
    CHECK(eventually([&] { return pair.closed[0].load() != -2 && pair.closed[1].load() != -2; }));
    CHECK(pair.entered.load() == 2);
    CHECK(pair.closed[0].load() == 0 && pair.closed[1].load() == 0);
    nts_status st{};
    CHECK(nts_status_get(pair.nodes[0], &st, err, sizeof err) == -1 && has(err, "is closed"));
    CHECK(nts_status_get(pair.nodes[1], &st, err, sizeof err) == -1 && has(err, "is closed"));
}

void certificate() {
    fs::path work = fresh("certificate");
    Control control(true);
    CHECK(control.ok);
    if (!control.ok) {
        return;
    }
    char url[256];
    char err[512];
    CHECK(nts_test_control_untrusted_url(url, sizeof url, err, sizeof err) == 0);
    int node = start(work / "state", "nexus-cert", url);
    CHECK(node > 0);
    if (node <= 0) {
        return;
    }
    nts_status st{};
    CHECK(waitState(node, NTS_STATE_FAILED, st));
    CHECK(st.reason == NTS_REASON_CERTIFICATE);
    CHECK(closeNode(node));
}

// By hand only (see the top of the file): a real start, which reaches Tailscale's hosted control server.
int startNeedsLogin(const char* dir) {
    char err[1024];
    nts_start_params p = params(dir, "nexus-layer1-check");
    int node = nts_start(&p, err, sizeof err);
    if (node == 0) {
        std::fprintf(stderr, "start_needs_login: %s\n", err);
        return 1;
    }
    nts_status st{};
    bool asked = waitFor(
        node, [](const nts_status& s) { return s.state == NTS_STATE_NEEDS_LOGIN || s.state == NTS_STATE_READY; }, st);
    std::printf("start_needs_login: %s (%s)\n", nts_state_name(st.state), nts_reason_name(st.reason));
    if (nts_close(node, err, sizeof err) != 0) {
        std::fprintf(stderr, "start_needs_login: %s\n", err);
        return 1;
    }
    return asked ? 0 : 1;
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"version", version},
    {"licences_unit", licencesUnit},
    {"names", names},
    {"handles", handles},
    {"start_checks", startChecks},
    {"panics_unit", panicsUnit},
    {"licences", licences},
    {"state_dir", stateDir},
    {"refusals", refusals},
    {"refuses_inherited_key", refusesInheritedKey},
    {"lock", lockInProcess},
    {"lock_two_process", lockTwoProcess},
    {"panics", panics},
    {"log_cap", logCap},
    {"quiet", quiet},
    {"unreachable", unreachable},
    {"certificate", certificate},
    {"watch", watch},
    {"threads", threads},
    {"sign_in", signIn},
    {"tailnet", tailnet},
    {"restart", restart},
    {"expiry", expiry},
    {"control_down", controlDown},
    {"auth_key", authKey},
    {"long_sign_in", longSignIn},
    {"control_error", controlError},
    {"control_blackhole", controlBlackhole},
    {"close_each_other", closeEachOther},
};

}  // namespace

int main(int argc, char** argv) {
    self = fs::absolute(argv[0]).string();
    if (argc == 3 && std::strcmp(argv[1], "start_needs_login") == 0) {
        return startNeedsLogin(argv[2]);
    }
    if (argc == 4 && std::strcmp(argv[1], "hold_lock") == 0) {
        return holdLock(argv[2], argv[3]);
    }
    if (argc == 2 && std::strcmp(argv[1], "quiet_child") == 0) {
        quietChild();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "long_sign_in_child") == 0) {
        longSignInChild();
        return failures == 0 ? 0 : 1;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: tailscale_test <group> | start_needs_login <dir>; the groups:");
        for (const Group& g : kGroups) {
            std::fprintf(stderr, " %s", g.name);
        }
        std::fprintf(stderr, "\n");
        return 2;
    }
    bool ran = false;
    for (const Group& g : kGroups) {
        if (std::strcmp(argv[1], g.name) == 0) {
            g.run();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "tailscale_test: no group %s\n", argv[1]);
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "tailscale_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("tailscale_test: ok\n");
    return 0;
}
