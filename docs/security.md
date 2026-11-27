# Security: the OS session, provider accounts and the server link

## Access to the desktop client

The signed-in operating-system session is sufficient to use Nexus. The desktop client does not add a Touch ID,
password or Windows Hello check. Command-backtick reveals the overlay directly, and `--windowed` opens directly.
Launching the overlay still shows nothing and takes no focus until its hotkey is pressed.

This is the owner’s 2026-11-26 decision, replacing the earlier mandatory per-client unlock. A live desktop client
creates no client lock, authenticator, idle-lock timer or session-lock watcher. Sleep, hiding the overlay and idle
time do not introduce a separate Nexus authentication step. The operating system controls access to its desktop
session; anyone who can use that session can use Nexus.

## Provider accounts

Provider sign-in remains separate. Nexus reuses a saved login and renews it where the provider supports renewal.
An account that needs sign-in still uses the in-app account card and the provider’s browser flow. See
[accounts](accounts.md) and [secrets](secrets.md) for account selection, cancellation and credential handling.
Removing the client unlock does not sign an account out or delete its saved login.

## The server link

The desktop window reaches `nexus-server` through SSH, using the person’s SSH keys and `known_hosts`. The bootstrap
hands back a UDP port and session key; mosh then carries the link encrypted and authenticated with that key
(AES-128-OCB). The server authenticates the machine and account that can connect over SSH. It does not depend on a
client-side unlock claim. Provider credentials and SSH authentication are unchanged by the client access decision.

## Retained lock fixtures

`--screenshot` draws a state file or the demo, never the live window. `--locked` additionally draws the historical
lock-screen fixture and is accepted only with `--screenshot`.

The isolated lock component (`src/client_lock.h`, `src/window_lock.h`), its platform adapters and its model
(`models/ClientLock.tla`) remain covered by their unit tests and implementation traces. Those tests describe the
retained component, not a lock enforced by the live desktop client. Release builds still reject the test-only
stand-in authenticator (`nexus_no_test_authenticator`).
