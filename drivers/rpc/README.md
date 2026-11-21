# Nexus harness drivers

Install the Python 3.11+ package into the server policy environment with
`/opt/nexus/venv/bin/python -m pip install ./drivers/rpc`. The adapters use the
installed vendor executables. The server and client binaries can keep running
while the package or driver table is updated.

Each command in the server's `drivers` file selects a protocol adapter and an absolute
path to the harness program. For example:

```text
driver codex /opt/nexus/venv/bin/python -I -m nexus_policy friend-driver /srv/nexus/profile {friend} {harness} /opt/nexus/venv/bin/python -I -m nexus_rpc.driver {harness} --binary /usr/local/bin/codex --account work
friend reviewer codex /srv/review
```

The server supplies the session's actual account, working directory and resume ID.
Configure a linked subscription account and instance in the server's accounts and
friends files. The program's home is a private server-owned directory at
`~/.nexus/server/harness/<harness>/<account>`. The driver starts the program with
a fresh environment. A missing home or a rejected credential produces a Nexus error.
The new adapters require an unlimited friend. They decline the server's directory
limit and keep-out flags at startup.

| Harness | Program and transport | Startup instructions | Subscription evidence before sending |
| --- | --- | --- | --- |
| Codex | `codex app-server --stdio` | Developer instructions on thread start and resume | ChatGPT account and plan from `account/read` |
| Antigravity | `agy acp --stdio` | Native system-prompt append on session creation and load | Antigravity tier on the session |
| Grok | `grok agent --no-leader stdio` | Named ACP context resource alongside the person's message | Grok account billing extension returns a subscription tier |
| Muse | `muse serve --provider meta` | Opening text part followed by the person's text, with the original prompt as the display text | MSP `usage/read` reports a subscription tier |

Each adapter resumes the specific conversation from the server. A missing conversation
starts a fresh one with the same identity. A stop ends the provider process for the
current turn. Nexus controls the next prompt and can restart the driver.

**Deployment status.** The built-in browser sign-in and credential isolation implemented
for Claude do not yet enroll accounts for these four adapters. In particular, Antigravity
can restore a login from the OS keyring, and Muse may have no subscription observation
on a fresh host. A separate HOME by itself does not isolate an OS keyring or deny a
model's tools access to credential files. Keep these adapters in an isolated deployment
until account enrollment and those boundaries have been verified for the intended host.

Run `python3 -m unittest discover -s drivers/rpc -v` from the repository root. The
tests run real adapter processes and scripted vendor peers with temporary account homes.
