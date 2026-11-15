// Tests of the Tailscale shim's C API (src/tailscale/nts.h), linked as nexus links it (cmake/tailscale.cmake): the
// c-archive on macOS and Linux, the DLL through its MSVC import library on Windows, with the Windows 10 manifest. It
// links the shim's test build, which adds the hooks of nts_test_hooks.h and is otherwise nexus's.
// Groups:
//   version      (unit) the versions the shim reports are the pins: tailscale.com v1.104.7 and go1.27.4
//   state_dir    (functional) the state directory and its log file on the real filesystem, in a directory of the
//                test's own: created owner-only (0700 and 0600; on Windows a protected ACL granting only this user), an
//                existing one tightened
//   refusals     (functional) every refusal that stops before any network: a null, empty or relative path, a file in
//                the way, an unwritable parent (skipped, saying so, when this user can write to it anyway), a start
//                while a tsnet sign-in variable is set, a handle that is not a node, and the reason cut to the caller's
//                buffer
//   refuses_inherited_key  (functional) with TS_AUTHKEY set from the process's start (ctest's ENVIRONMENT), a start
//                refuses
//   lock         (functional) a second start, or a prepare, on a state directory a node of this process holds is
//                refused, before anything in it is opened; closing the node frees it
//   lock_two_process  (functional) the same across processes: a child (hold_lock) holds the directory, and this
//                process's start and prepare are refused until the child closes its node
//   panics       (functional) a panic injected into each export is stopped there: the call fails with a reason, what
//                it held is given back, and the panic's text goes to the running node's log
//   log_cap      (functional) a node's log past 1 MiB moves to nexus-tailscale.log.1 and a new one begins
//   quiet        (functional) a child (quiet_child) runs a node's start and close, the library loggers and the panics
//                group with its stdout and stderr in files: both stay empty
//   start_needs_login <dir>  by hand only, never from ctest or CI: starts a node on a scratch state directory, which
//                contacts Tailscale's control server, waits until it asks to sign in, prints the backend state, and
//                closes it. It never prints the sign-in URL, which goes only to the state directory.
// lock, lock_two_process, panics, log_cap and quiet start offline nodes (nts_test_start_offline): sockets on this machine
// only, a control server on 127.0.0.1 that refuses at once and magicsock's UDP socket, never another machine; they
// wait on the clock only for the child of lock_two_process. Every other group uses no socket and no clock.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

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

// A start of an offline node (nts_test_start_offline); its handle, or 0 with the reason in why.
int startOffline(const fs::path& dir, std::string& why) {
    char err[512];
    int node = nts_test_start_offline(dir.string().c_str(), "nexus-test", err, sizeof err);
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
    std::string why = prepare(file);
    CHECK(has(why, "is not a directory"));
    why = prepare(file / "state");
    CHECK(has(why, "cannot create the state directory"));

    // A parent this user cannot write to, once the test has seen that this user really cannot (root, or an
    // administrator's backup privilege, writes anyway): otherwise skipped, saying so, since a start there would get as
    // far as tsnet and the network.
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
        // With no sign-in variable set, a start on an unwritable directory is refused by the state directory, before
        // tsnet.
        CHECK(nts_start((locked / "state").string().c_str(), "nexus-test", err, sizeof err) == 0);
        CHECK(has(err, "cannot create the state directory"));
    }
    unlock(locked);

    // A start refuses before anything else while a tsnet sign-in variable is set, naming it (never its value). The
    // directory is under a file, which no user can create a directory in, so that no start could get as far as the
    // network.
    for (const char* name : {"TS_AUTHKEY", "TS_AUTH_KEY", "TS_CLIENT_SECRET", "TS_CLIENT_ID", "TS_ID_TOKEN",
                             "TS_AUDIENCE", "TS_CONTROL_URL"}) {
        setEnv(name, "a-value-that-must-not-be-shown");
        CHECK(nts_start((file / "state").string().c_str(), "nexus-test", err, sizeof err) == 0);
        CHECK(has(err, name) && has(err, "interactively only") && !has(err, "a-value-that-must-not-be-shown"));
        setEnv(name, nullptr);
    }
    CHECK(nts_start((work / "named").string().c_str(), "", err, sizeof err) == 0 && has(err, "hostname"));
    CHECK(nts_start(nullptr, "nexus-test", err, sizeof err) == 0 && has(err, "state directory"));
    CHECK(!fs::exists(work / "named"));

    // Handles that are not nodes.
    CHECK(nts_close(0, err, sizeof err) == -1 && has(err, "not a started node"));
    CHECK(nts_close(12345, err, sizeof err) == -1 && has(err, "not a started node"));
    CHECK(nts_backend_state(12345, err, sizeof err) == -1 && has(err, "not a started node"));

    // The reason is cut to the caller's buffer and always ends; a null buffer or a size of 0 is allowed.
    char tiny[8];
    std::memset(tiny, 'x', sizeof tiny);
    CHECK(nts_close(1, tiny, sizeof tiny) == -1 && std::strlen(tiny) == 7);
    CHECK(nts_close(1, nullptr, 0) == -1);
    tiny[0] = 'x';
    CHECK(nts_close(1, tiny, 0) == -1 && tiny[0] == 'x');
}

// Run with TS_AUTHKEY in the environment from the process's start (ctest sets it), as a key inherited from a shell would
// be, rather than set by the program: a start refuses, naming it and never its value.
void refusesInheritedKey() {
    fs::path work = fresh("inherited");
    char err[512];
    CHECK(std::getenv("TS_AUTHKEY") != nullptr);
    // Under a file, as in refusals, so that no start could get as far as the network.
    fs::path file = work / "file";
    {
        std::ofstream(file) << "in the way";
    }
    CHECK(nts_start((file / "state").string().c_str(), "nexus-test", err, sizeof err) == 0);
    CHECK(has(err, "TS_AUTHKEY") && has(err, "interactively only") && !has(err, "a-value-that-must-not-be-shown"));
}

// A state directory is one node's: a second start or a prepare on it is refused while a node of this process holds
// it, however the path is spelled, and allowed again once the node closes. Only offline starts: were the refusal
// missing, nothing would reach the network.
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
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!fs::exists(marker) && std::chrono::steady_clock::now() < deadline) {
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

const char* const kPanicReason = "stopped by an internal error (a Go panic)";

// A panic injected into each export is stopped there.
void panics() {
    fs::path work = fresh("panics");
    fs::path dir = work / "state";
    std::string path = dir.string();
    char err[512];

    nts_test_inject_panic("nts_prepare");
    CHECK(nts_prepare(path.c_str(), err, sizeof err) == -1 && has(err, kPanicReason));
    nts_test_inject_panic("nts_start");
    CHECK(nts_start(path.c_str(), "nexus-test", err, sizeof err) == 0 && has(err, kPanicReason));
    nts_test_inject_panic("nts_test_start_offline");
    CHECK(nts_test_start_offline(path.c_str(), "nexus-test", err, sizeof err) == 0 && has(err, kPanicReason));
    nts_test_inject_panic("nts_backend_state");
    CHECK(nts_backend_state(1, err, sizeof err) == -1 && has(err, kPanicReason));
    nts_test_inject_panic("nts_close");
    CHECK(nts_close(1, err, sizeof err) == -1 && has(err, kPanicReason));
    nts_test_inject_panic("nts_version");
    CHECK(nts_version() != nullptr && std::string(nts_version()) == "tailscale.com v1.104.7 go1.27.4");

    // A panic after the directory is held and its log opened gives both back.
    nts_test_inject_panic("nts_test_start_offline after open");
    CHECK(nts_test_start_offline(path.c_str(), "nexus-test", err, sizeof err) == 0 && has(err, kPanicReason));
    nts_test_inject_panic(nullptr);
    std::string why;
    int node = startOffline(dir, why);
    CHECK(node > 0);
    if (node <= 0) {
        std::fprintf(stderr, "panics: %s\n", why.c_str());
        return;
    }
    // With a node running, the panic's text goes to its log.
    nts_test_inject_panic("nts_backend_state");
    CHECK(nts_backend_state(node, err, sizeof err) == -1 && has(err, kPanicReason));
    nts_test_inject_panic(nullptr);
    char state[64];
    CHECK(nts_backend_state(node, state, sizeof state) == 0);
    CHECK(closeNode(node));
    CHECK(has(readFile(dir / "nexus-tailscale.log"), "nts test panic injected at nts_backend_state"));
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

// The child of quiet, run with its stdout and stderr in files: a node's start and close, the library loggers before,
// during and after it, and the panics group. It prints nothing unless a check fails.
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
    panics();
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

// By hand only (see the top of the file): a real start, which reaches Tailscale's control server.
int startNeedsLogin(const char* dir) {
    char err[1024];
    int node = nts_start(dir, "nexus-layer0-check", err, sizeof err);
    if (node == 0) {
        std::fprintf(stderr, "start_needs_login: %s\n", err);
        return 1;
    }
    std::string state;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        char out[256];
        if (nts_backend_state(node, out, sizeof out) == 0) {
            state = out;
            if (state == "NeedsLogin" || state == "Running") {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::printf("start_needs_login: backend state %s\n", state.empty() ? "(none)" : state.c_str());
    if (nts_close(node, err, sizeof err) != 0) {
        std::fprintf(stderr, "start_needs_login: %s\n", err);
        return 1;
    }
    return state == "NeedsLogin" || state == "Running" ? 0 : 1;
}

struct Group {
    const char* name;
    void (*run)();
};

const Group kGroups[] = {
    {"version", version},
    {"state_dir", stateDir},
    {"refusals", refusals},
    {"refuses_inherited_key", refusesInheritedKey},
    {"lock", lockInProcess},
    {"lock_two_process", lockTwoProcess},
    {"panics", panics},
    {"log_cap", logCap},
    {"quiet", quiet},
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
    if (argc != 2) {
        std::fprintf(stderr, "usage: tailscale_test <version|state_dir|refusals|refuses_inherited_key|lock|"
                             "lock_two_process|panics|log_cap|quiet> | start_needs_login <dir>\n");
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
