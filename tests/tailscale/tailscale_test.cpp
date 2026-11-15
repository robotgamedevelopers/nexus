// Tests of the Tailscale shim's C API (src/tailscale/nts.h), linked as nexus will link it (cmake/tailscale.cmake): the
// c-archive on macOS and Linux, the DLL through its MSVC import library on Windows, with the Windows 10 manifest.
// Groups:
//   version      (unit) the versions the shim reports are the pins: tailscale.com v1.104.1 and go1.27.1
//   state_dir    (functional) the state directory and its log file on the real filesystem, in a directory of the
//                test's own: created owner-only (0700 and 0600; on Windows a protected ACL granting only this user), an
//                existing one tightened
//   refusals     (functional) every refusal that stops before any network: a null, empty or relative path, a file in
//                the way, an unwritable parent, a start while a tsnet sign-in variable is set, a handle that is not a
//                node, and the reason cut to the caller's buffer
//   refuses_inherited_key  (functional) with TS_AUTHKEY set from the process's start (ctest's ENVIRONMENT), a start
//                refuses
//   start_needs_login <dir>  by hand only, never from ctest or CI: starts a node on a scratch state directory, which
//                contacts Tailscale's control server, waits until it asks to sign in, prints the backend state, and
//                closes it. It never prints the sign-in URL, which goes only to <dir>/nexus-tailscale.log.
// No group uses a socket or the network; only start_needs_login uses the clock.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "nts.h"

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

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
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
    CHECK(text == "tailscale.com v1.104.1 go1.27.1");
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

    // A parent this user cannot write to.
    fs::path locked = work / "locked";
    fs::create_directories(locked);
    CHECK(lock(locked));
    why = prepare(locked / "state");
    CHECK(has(why, "cannot create the state directory") && has(why, "fix the permissions of its parent"));
    CHECK(!fs::exists(locked / "state"));

    // A start refuses before anything else while a tsnet sign-in variable is set, naming it (never its value); the
    // directory is under the locked parent as well, so that no start could get as far as the network.
    for (const char* name : {"TS_AUTHKEY", "TS_AUTH_KEY", "TS_CLIENT_SECRET", "TS_CLIENT_ID", "TS_ID_TOKEN",
                             "TS_AUDIENCE", "TS_CONTROL_URL"}) {
        setEnv(name, "a-value-that-must-not-be-shown");
        CHECK(nts_start((locked / "state").string().c_str(), "nexus-test", err, sizeof err) == 0);
        CHECK(has(err, name) && has(err, "interactively only") && !has(err, "a-value-that-must-not-be-shown"));
        setEnv(name, nullptr);
    }
    // With none set, a start on an unwritable directory is refused by the state directory, before tsnet.
    CHECK(nts_start((locked / "state").string().c_str(), "nexus-test", err, sizeof err) == 0);
    CHECK(has(err, "cannot create the state directory"));
    CHECK(nts_start((work / "named").string().c_str(), "", err, sizeof err) == 0 && has(err, "hostname"));
    CHECK(nts_start(nullptr, "nexus-test", err, sizeof err) == 0 && has(err, "state directory"));
    CHECK(!fs::exists(work / "named"));
    unlock(locked);

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
    fs::path locked = work / "locked";
    fs::create_directories(locked);
    CHECK(lock(locked));
    CHECK(nts_start((locked / "state").string().c_str(), "nexus-test", err, sizeof err) == 0);
    CHECK(has(err, "TS_AUTHKEY") && has(err, "interactively only") && !has(err, "a-value-that-must-not-be-shown"));
    unlock(locked);
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
};

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "start_needs_login") == 0) {
        return startNeedsLogin(argv[2]);
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: tailscale_test <version|state_dir|refusals|refuses_inherited_key> | "
                             "start_needs_login <dir>\n");
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
