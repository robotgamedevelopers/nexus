// Watch the installed application, so replacing it can update a compatible open window.
// Files, signing checks and dyld stay on the watcher thread.
#include "client_reload.h"
#include "client_module_version.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <atomic>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <cstdio>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>
#include <sys/stat.h>

namespace nexus {
namespace {
using Path = std::filesystem::path;

bool trustedPath(const Path& path, bool directory) {
    struct stat info{};
    return lstat(path.c_str(), &info) == 0 && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode)) &&
        (info.st_uid == getuid() || info.st_uid == 0) && !(info.st_mode & 0022);
}

NSDictionary* signingInfo(SecStaticCodeRef code) {
    CFDictionaryRef info = nullptr;
    if (SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info) != errSecSuccess) return nil;
    return CFBridgingRelease(info);
}

bool adHoc(NSDictionary* info) {
    return ([info[(__bridge NSString*)kSecCodeInfoFlags] unsignedIntValue] & kSecCodeSignatureAdhoc) != 0;
}

struct SigningIdentity {
    NSDictionary* host = nil;
    SecRequirementRef requirement = nullptr;

    SigningIdentity() {
        SecCodeRef resident = nullptr;
        if (SecCodeCopySelf(kSecCSDefaultFlags, &resident) != errSecSuccess) return;
        host = signingInfo(resident);
        if (host && !adHoc(host))
            SecCodeCopyDesignatedRequirement(resident, kSecCSDefaultFlags, &requirement);
        CFRelease(resident);
    }
    ~SigningIdentity() { if (requirement) CFRelease(requirement); }
};

bool validPackage(NSURL* url, const SigningIdentity& identity) {
    NSDictionary* host = identity.host;
    NSString* teamKey = (__bridge NSString*)kSecCodeInfoTeamIdentifier;
    if (!host || (!adHoc(host) && !identity.requirement)) return false;
    SecStaticCodeRef code = nullptr;
    OSStatus result = SecStaticCodeCreateWithPath((__bridge CFURLRef)url, kSecCSDefaultFlags, &code);
    if (result == errSecSuccess)
        result = SecStaticCodeCheckValidity(code, kSecCSStrictValidate | kSecCSCheckNestedCode, identity.requirement);
    if (result != errSecSuccess)
        std::fprintf(stderr, "nexus: updated application signature check returned %d\n", int(result));
    NSDictionary* candidate = result == errSecSuccess ? signingInfo(code) : nil;
    if (code) CFRelease(code);
    NSString* idKey = (__bridge NSString*)kSecCodeInfoIdentifier;
    if (![host[idKey] isEqual:candidate[idKey]]) return false;
    if (adHoc(host)) return adHoc(candidate);
    if (host[teamKey]) return [host[teamKey] isEqual:candidate[teamKey]];
    // A local signing certificate may have no Apple team identifier. Pin the leaf in that case.
    NSString* certificates = (__bridge NSString*)kSecCodeInfoCertificates;
    id leaf = [host[certificates] firstObject];
    return leaf && [leaf isEqual:[candidate[certificates] firstObject]];
}

// Verify and load the exact same copy, even if another installation happens in between.
struct PackageSnapshot {
    Path directory;
    ~PackageSnapshot() {
        std::error_code ignored;
        if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
    }
    NSURL* copy(const Path& app) {
        auto pattern = (Path(NSTemporaryDirectory().fileSystemRepresentation) / "nexus-module.XXXXXX").string();
        if (!mkdtemp(pattern.data())) return nil;
        directory = pattern;
        const auto target = [NSURL fileURLWithFileSystemRepresentation:(directory / "nexus.app").c_str()
                                                          isDirectory:YES relativeToURL:nil];
        const auto source = [NSURL fileURLWithFileSystemRepresentation:app.c_str() isDirectory:YES relativeToURL:nil];
        return [[NSFileManager defaultManager] copyItemAtURL:source toURL:target error:nil] ? target : nil;
    }
};

bool hashText(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
}

struct ClientReload::State {
    std::atomic<const ClientModule*> ready{nullptr};
    const SigningIdentity identity;
    dispatch_source_t watcher = nullptr;
    dispatch_semaphore_t stopped = nullptr;
    std::map<std::string, const ClientModule*> revisions;
    // The executable already contains its own renderer at launch.
    std::string checked = NEXUS_CLIENT_REVISION;
    ino_t checkedMarker = 0;

    void check(const Path& app, const std::function<void()>& wake) {
        const Path contents = app / "Contents", resources = contents / "Resources";
        const Path marker = resources / "client-module.txt";
        if (!trustedPath(app, true) || !trustedPath(contents, true) ||
                !trustedPath(resources, true) || !trustedPath(marker, false)) return;
        struct stat stamp{};
        if (lstat(marker.c_str(), &stamp) != 0) return;
        std::ifstream in(marker);
        std::string abi, revision;
        if (!std::getline(in, abi) || !std::getline(in, revision) || !hashText(abi) ||
                !hashText(revision) || (revision == checked && (checkedMarker == 0 || stamp.st_ino == checkedMarker))) return;
        checked = revision;
        checkedMarker = stamp.st_ino;
        if (abi != NEXUS_CLIENT_COMPATIBILITY) {
            std::fprintf(stderr, "nexus: native update needs a new client host; keeping the open window\n");
            return;
        }
        PackageSnapshot snapshot;
        NSURL* package = snapshot.copy(app);
        if (!package || !validPackage(package, identity)) {
            std::fprintf(stderr, "nexus: native update failed package verification\n");
            return;
        }
        const Path sealed = Path(package.fileSystemRepresentation) / "Contents";
        const Path directory = sealed / "PlugIns";
        const Path file = directory / ("nexus-client-" + revision + ".bundle");
        std::ifstream confirmed(sealed / "Resources" / "client-module.txt");
        std::string confirmedAbi, confirmedRevision;
        if (!std::getline(confirmed, confirmedAbi) || !std::getline(confirmed, confirmedRevision) ||
                confirmedAbi != abi || confirmedRevision != revision ||
                !trustedPath(directory, true) || !trustedPath(file, false)) return;
        if (const auto found = revisions.find(revision); found != revisions.end()) {
            ready.store(found->second);
            wake();
            return;
        }
        void* library = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
        if (!library) {
            std::fprintf(stderr, "nexus: could not load the native client module: %s\n", dlerror());
            return;
        }
        const auto load = reinterpret_cast<GetClientModule>(dlsym(library, "nexusClientModule"));
        const auto* api = load ? load() : nullptr;
        if (!api || !api->compatibility || abi != api->compatibility || !api->revision || revision != api->revision || !api->begin || !api->event || !api->build || !api->wantsFrame) {
            dlclose(library);
            std::fprintf(stderr, "nexus: native module has an incompatible entry point\n");
            return;
        }
        // Previous revisions stay mapped: existing Prax nodes can retain callbacks into them.
        revisions.emplace(revision, api);
        ready.store(api);
        std::fprintf(stderr, "nexus: native client module %.12s ready\n", revision.c_str());
        wake();
    }
};

ClientReload::ClientReload() : state_(std::make_unique<State>()) {}
ClientReload::~ClientReload() { stop(); }

void ClientReload::start(std::function<void()> wake) {
    if (state_->watcher) return;
    const Path app([[NSBundle mainBundle] bundleURL].fileSystemRepresentation);
    // The installer atomically replaces the app directory. Watch its parent, which survives the swap.
    const int directory = ::open(app.parent_path().c_str(), O_EVTONLY | O_CLOEXEC);
    if (directory < 0) return;
    auto queue = dispatch_queue_create("nexus.client-update", DISPATCH_QUEUE_SERIAL);
    auto watcher = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, directory,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_RENAME, queue);
    if (!watcher) { ::close(directory); return; }
    state_->watcher = watcher;
    auto stopped = state_->stopped = dispatch_semaphore_create(0);
    dispatch_block_t check = ^{
        @autoreleasepool { state_->check(app, wake); }
    };
    dispatch_source_set_event_handler(watcher, check);
    dispatch_source_set_cancel_handler(watcher, ^{
        ::close(directory);
        dispatch_semaphore_signal(stopped);
    });
    dispatch_resume(watcher);
    dispatch_async(queue, check);
}

void ClientReload::stop() {
    if (!state_->watcher) return;
    dispatch_source_cancel(state_->watcher);
    dispatch_semaphore_wait(state_->stopped, DISPATCH_TIME_FOREVER);
    state_->watcher = nullptr;
    state_->stopped = nullptr;
}
const ClientModule* ClientReload::current() const { return state_->ready.load(); }
}
