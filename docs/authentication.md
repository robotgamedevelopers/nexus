# Authentication owned by Nexus

The requested account-switching behavior applies to every harness and every client platform. Choosing another
account signs out the current Nexus account before starting its replacement. Nexus owns preparation, presentation,
cancellation, verification and completion. No manual browser logout or extra confirmation is part of a switch.

Use an isolated in-app authentication session where the provider supports it. When the provider requires system
authentication, open that automatically and keep its lifecycle attached to the Nexus attempt. Do not disguise an
embedded browser to evade a provider's restrictions. A system window opening, a callback arriving, or a helper
exiting is not proof of login: the server must verify and save the selected account's grant before reporting success.
The person's ordinary browser session is independent of an authentication session owned by Nexus.

A server may be a headless Linux machine. It never opens authentication UI or requires a desktop, display server,
Safari, or a browser installation. Client and server operating systems are independent: browser interaction happens
on the client, and verified harness credentials stay on the server. A device-code helper or callback listener on the
server is reached through the authenticated Nexus connection, not through the public network.

## Contracts

The server owns the selected account and saved grants. The client sends explicit requests and displays server state.
A provider adapter advertises supported presentation and transport capabilities; the UI must not branch on a harness
name. The common lifecycle is prepare, sign out, select, authenticate, verify, complete, with explicit refused,
cancelled and disconnected outcomes. A failed login never silently restores the account that was signed out.

Every attempt has an identity. Replies and UI callbacks from a replaced attempt are ignored. Cancellation ends its
presentation and asks the server to stop the exact owned enrollment, including before a browser URL exists. A grant
already received is preserved under the existing one-chance rule. A later explicit sign-out may remove it.

Provider pages, device codes and callbacks travel only inside the authentication components. Never put them in
logs, transcripts, screenshots captured by tests against real providers, command-line arguments, or AI-visible files.
A remote helper page must be reached through an authenticated channel to its server; its localhost URL cannot be
opened directly on a different machine. Browser fallback must not consume a helper page's one-time claim a second
time or drop the page that holds a pending device-code response.

## Current implementation gaps

This document specifies the target, not delivered support:

- Login preparation currently has a Claude-specific manual browser logout phase. Replace it with the shared
  presenter once an automatically managed authentication session is available.
- Remote callback forwarding exists for Claude. Codex, Antigravity, Grok and Muse use one-time local helper pages
  and currently require a loopback connection. They need authenticated forwarding of their helper UI and requests.
- Attempt-scoped cancellation now covers all harnesses, including before their URL is available. Both client and
  server need the new protocol capability; older servers retain their callback-based cancellation path.
- Prax's pinned webview API exposes isolated WebKit and WebView2 views, but no navigation or new-window policy hooks.
  An auth presenter needs those hooks to hand provider-required authentication to the system without guessing from
  page text or injecting scripts into credential pages. Keep Prax pinned and unmodified; use a documented native
  host capability where necessary and record the missing Prax API.
- The repository builds native macOS and Windows desktop clients. Linux currently builds server and client logic,
  with no native GUI/app backend. Linux GUI and authentication presentation require that platform work first.

## System authentication constraints

System authentication is a provider-required fallback, not evidence that browser cookies were cleared.
[Microsoft's desktop OAuth guidance](https://learn.microsoft.com/en-us/windows/apps/develop/security/oauth2)
states that its general OAuth manager uses the system browser. A platform adapter must explicitly establish fresh
provider authentication through supported mechanisms; it must not label a normal browser launch as a session reset.
If it cannot establish the reset, the switch stays stopped with a reason instead of silently reusing the old identity.

## Qualification

For each available harness and platform, exercise switching from a saved account, selecting an unsigned account,
retrying after browser cancellation, cancelling before URL delivery, switching while a callback arrives, wrong-email
verification, disconnect/reconnect, server refusal, and completion while the overlay is hidden. Test delayed and
stale callbacks with fake transports. Bind lifecycle implementations to TLA+ traces with negative witnesses.

Real-provider qualification must verify that the selected account can make a request after enrollment and that a
failed attempt leaves it selected. Unit and model checks alone are not evidence of working provider authentication.
Mac, Windows and Linux qualification are separate; an unimplemented platform is not a passing platform.
