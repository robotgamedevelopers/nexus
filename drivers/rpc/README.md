# Nexus harness drivers

Install the Python 3.11+ package into the server policy environment with
`/opt/nexus/venv/bin/python -m pip install ./drivers/rpc`. The adapters use the
installed vendor executables. The server and client binaries can keep running
while the package or driver table is updated.

Each command in the server's `drivers` file selects a protocol adapter and an absolute
path to the harness program. For example:

```text
driver codex /opt/nexus/venv/bin/python -I -m nexus_policy friend-driver /srv/nexus/profile {friend} {harness} /opt/nexus/venv/bin/python -I -m nexus_rpc.driver {harness} --binary /usr/local/bin/codex --account work
signin codex /opt/nexus/venv/bin/python -I -m nexus_rpc.enroll codex --binary /usr/local/bin/codex
friend reviewer codex /srv/review
```

The server supplies the session's actual account, working directory and resume ID.
Configure a linked subscription account and instance in the server's accounts and
friends files. The program's home is a private server-owned directory at
`~/.nexus/server/harness/<harness>/<account>`. The driver starts the program with
a fresh environment. A missing home or a rejected credential produces a Nexus error.
Each installed adapter declares which limits it enforces. Codex checks the effective
native permission profile on both start and resume. Antigravity, Grok and Muse
use dialogue sessions in a private working directory. The deployment must use a
vendor version and operating system supported by that adapter.

| Harness | Program and transport | Startup instructions | Subscription evidence before sending |
| --- | --- | --- | --- |
| Codex | `codex app-server --stdio` | Developer instructions on thread start and resume | ChatGPT account and plan from `account/read` |
| Antigravity | `agy acp --stdio` | Native system-prompt append on session creation and load | Antigravity tier on the session |
| Grok | `grok agent --no-leader stdio` | Named ACP context resource alongside the person's message | Grok account billing extension returns a subscription tier |
| Muse | `muse serve --provider meta` | Opening text part followed by the person's text, with the original prompt as the display text | MSP `account/read` and `usage/refresh` confirm an active subscription |

Each adapter resumes the specific conversation from the server. A missing conversation
starts a fresh one with the same identity. A stop ends the provider process for the
current turn. Nexus controls the next prompt and can restart the driver.

### Enroll a named account

Add a `signin` line for each provider alongside its `driver` line. Both lines
must point to the same installed vendor executable. Link accounts to friends in
the order the fork should offer them. A signed-out account marked
`"sign_in_on_use": true` defers its sign-in offer until use. In the selected friend's tab,
press **Enter** or click **Sign in to** to open that account’s browser flow. Its draft stays in the prompt.

The local Nexus window opens a private page for that account. Complete the vendor's
browser authorization there. Antigravity asks you to paste Google's return code
into that page. A remote window can keep chatting on an enrolled account. Initial
enrollment for these four providers needs a window connected to the server over
loopback. The server probes the account-specific enrollment receipt on restart.
The driver checks the actual subscription before sending the prompt.

Run `python3 -m unittest discover -s drivers/rpc -v` from the repository root. The
tests run real adapter processes and scripted vendor peers with temporary account homes.
