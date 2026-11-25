// The client lock's system side (client_lock.h): the owner's authentication, and the OS's events that lock which Prax's
// lib/app does not report. Each system's code is beside this file: lock_mac.mm (LocalAuthentication, AppKit) and
// lock_win.cpp (Windows Hello through UserConsentVerifier, Win32).
//
// Prax reports two of the events itself, and main.cpp takes them from it: PrAppEventKind_Suspend (the machine is about
// to sleep: NSWorkspaceWillSleepNotification, PBT_APMSUSPEND) and PrAppEventKind_ScreenLocked (com.apple.screenIsLocked,
// WTS_SESSION_LOCK). The rest are watched here, as requests for Prax's author: on macOS the displays sleeping
// (NSWorkspaceScreensDidSleepNotification) and the session switched away (NSWorkspaceSessionDidResignActiveNotification);
// on Windows the console or remote session disconnected or logged off (WTS_CONSOLE_DISCONNECT, WTS_REMOTE_DISCONNECT,
// WTS_SESSION_LOGOFF) and the display turned off (GUID_CONSOLE_DISPLAY_STATE, which modern standby sends where it sends
// no PBT_APMSUSPEND).
//
// Everything here is a seam: tests and the test build (NEXUS_TEST_AUTHENTICATOR) give a fake; the app gives the
// system's. Answers and events may come on any thread: each is handed to `post` (main.cpp: a LockMailbox and a wake of
// the window's thread), and nothing else.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "client_lock.h"
#include "pr/surface.h"

namespace nexus {

using LockPost = std::function<void(LockNotice)>;

class LockAuthenticator {
public:
    virtual ~LockAuthenticator() = default;
    // How the lock screen names the system's authentication: "Touch ID or your password", "Windows Hello".
    virtual std::string method() const = 0;
    // Asks the system to authenticate the device's owner for attempt `attempt`, over the window behind `surface`. The
    // answer (granted, or which refusal, with the system's own words) is posted once, from any thread. A request that
    // cannot even start is posted as Unavailable with why: never nothing.
    virtual void request(std::uint64_t attempt, const PrSurface& surface, LockPost post) = 0;
    // Ends a request still up (a relock overtook it): the system's prompt goes away; its answer, if one still comes, is
    // stale and changes nothing (ClientLock::answer).
    virtual void cancel() = 0;
};

// This system's authenticator: LocalAuthentication's device-owner policy (Touch ID, an Apple Watch, or the login
// password) on macOS; Windows Hello (face, fingerprint or PIN) on Windows.
std::unique_ptr<LockAuthenticator> systemLockAuthenticator();

// The events Prax does not report (above), each posted as a relock while the returned object lives. Started on the
// window's thread, once the app runs.
class LockEvents {
public:
    virtual ~LockEvents() = default;
};
std::unique_ptr<LockEvents> watchLockEvents(LockPost post);

} // namespace nexus
