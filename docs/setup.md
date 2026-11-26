# Set up nexus for your human

You are the AI the human already uses. Run these steps from this checkout. The goal is for the human to open nexus, type to you and get a reply. Use the current harness, workspace and chosen subscription. Ask the human to complete any interactive provider sign-in themselves.

## 1. Identify the installation

Record the provider, the absolute workspace path and whether the server runs on this machine or another host. A fresh conversation with the same harness is fine. Reuse a prior conversation only when you have its explicit provider resume handle. The human can start without naming an AI or providing a GitHub repository.

Check for an existing server with `nexus-server status` and inspect its configured service ownership with `nexus-server services status`. Preserve its data, profiles, accounts, service definitions and signing identity. For an existing fleet, use the [service transfer procedure](service-migration.md) before changing which process owns Redis or Sprint.

## 2. Build the client, server and selected driver

Use the Apple silicon macOS build for the desktop client. Install the prerequisites in the [build guide](README.md#build). From the repository root run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure -R '^(window_input|sync_unit)$'
```

Install the server and `tools/nexus-receive` on the server login's PATH. Use the [server build and supervision guide](builds.md) to place the build, select it and register its launchd or systemd unit. Keep the client app at a stable path. Stage the driver for the harness you are using. Claude needs the compiled Node driver. Codex, Antigravity, Grok and Muse use the Python RPC adapters. Use the provider's actual entry point and account requirements. [Claude driver](../drivers/claude/drivers.example) · [RPC drivers](../drivers/rpc/README.md)

## 3. Provision local Nova services

Install the matching nova-tools and nova-sprint executables for the server's OS and architecture. Verify release assets against the project's release checksums. Resolve dependencies and storage **on the target server**. Scope provisioning to this installation. A fleet-wide play changes other machines.

- **Registry.** Initialize persistent PostgreSQL and the nova-config schema. Supply separate administrative and runtime credentials through the local secrets store. Verify a read using the runtime account, even if the AI list is empty. [nova-config setup](https://github.com/mas-bandwidth/nova-tools/tree/main/cmd/nova-config)
- **Message bus.** Initialize persistent Redis with an authenticated local endpoint. Configure the server's bus launcher with the runtime seat. Verify an isolated sender/recipient exchange. Use disposable identities so setup traffic stays out of real chats. [nova-bus setup](https://github.com/mas-bandwidth/nova-tools/tree/main/cmd/nova-bus)
- **Sprint.** Provision its database, apply the release's store migration and backup verification, and configure its scheduler and dashboard address. Enable the Dashboard by default. Leave the work queue stopped until the human starts a sprint. [Sprint setup](https://github.com/mas-bandwidth/nova-sprint/blob/main/docs/README.md)
- **Ownership.** Declare the foreground services under nexus-server in its private `services.json`. Give Sprint services `"feature": "sprint"`. Run `nexus-server services check`, `nexus-server services reload` and `nexus-server services status`. Check the bus and the dashboard's `/api/sprint` while the client is closed. [Manifest format and examples](service-migration.md)

## 4. Connect the human to the installing harness

Bind the current harness, subscription account and workspace on the server. Configure the provider's sign-in helper. Set `server <host>` in `~/.nexus/client/config` to the selected server, preserving other settings. When upgrading a login launcher with an explicit host, transfer that host to the config before replacing the launcher. Open nexus normally and verify the configured tabs appear. Use an existing verified nexus account when available. If enrollment is required, hand the provider dialog to the human. Keep credential files and tokens out of the chat.

In `~/.nexus/server/drivers`, keep the other entries and add the primary route. For example, for Codex on macOS, substitute the verified account name, actual workspace, Python with the nexus policy and RPC modules installed, and the Codex executable:

```text
primary codex personal /absolute/workspace
primary-driver codex ["/absolute/venv/bin/python","-I","-m","nexus_policy","primary-driver","codex","/absolute/venv/bin/python","-I","-m","nexus_rpc.driver","codex","--binary","/absolute/bin/codex"]
```

For Claude, use `claude` in both records and pass the absolute Node and compiled Claude driver paths after `primary-driver","claude"`. Add `primary-resume <provider-session-id>` only when resuming that conversation. Load the driver table before opening the client.

Apply the [keyboard client profile](policy.md#start-with-the-keyboard-driven-client). Open the conversation for the installing harness and have the human send their first message. Verify the reply arrives in that same tab. Offer AI administration only when asked.

## 5. Report what works

Tell the human the app location, overlay shortcut, selected harness and workspace. Report the observed server, bus, dashboard and conversation results. If something failed, identify the failed check and continue from it after fixing the cause. A successful compile alone leaves the installation unverified.

## Changing the Sprint coordinator

Read the current coordinator before acting. Taking another AI's seat requires **explicit permission from the human for that handoff**. An AI's request, an unavailable coordinator and a reason string do not grant permission. Once approved, the incoming coordinator runs the command from its own authenticated Sprint session:

```sh
nova-sprint up --as <incoming-name> --take-over --reason "<human-approved reason>"
```

The previous coordinator may be offline. Sprint records the change, returns any cards in that coordinator's hand to the queue and sends the former coordinator a notice with the reason when possible. Confirm the new coordinator on the Dashboard. If the intended recipient already owns the seat, the handoff is complete.

## Deploying a new build to a running server

A running nexus-server is replaced through placement and handover, never by stopping it by hand. Each step uses
`nexus-server`'s own verbs (`nexus-server builds help`, `nexus-server handover help`). Below, `OLD` is the build
serving now (`~/.nexus/server/current`) and `NEW` is the folder you assemble.

1. Build the server, the app and the drivers that changed from one commit, after its tests pass.
2. Assemble `NEW` with only what `builds place` takes: `bin/` (with `bin/nexus-server`), `nexus.app/`, and
   `drivers/`. Copy any driver that did not change from `OLD`. Then write the formats file:
   `NEW/bin/nexus-server builds formats > NEW/formats`.
3. Compare the file lists: `diff <(cd OLD && find . | sort) <(cd NEW && find . | sort)`. Every line that differs
   should be a change you meant to make.
4. Hash the tree and write the evidence that its tests passed:
   ```sh
   H=$(NEW/bin/nexus-server builds hash NEW)
   printf 'nexus-test-evidence 1\ntree %s\nresult pass\nrun <commit: the suites run and their counts>\n' "$H" > evidence
   ```
5. Place it: `NEW/bin/nexus-server builds place NEW --evidence evidence`. The build is now
   `~/.nexus/server/builds/$H`, read-only and checked against its manifest.
6. Hand over: `~/.nexus/server/current/bin/nexus-server handover $H`. If the new build does not reach SERVING, the
   handover rolls back to the previous build by itself (exit status 5). `handover status` shows the current and
   previous builds.
7. Check that the server answers, that the AI sessions came back with the same AIs, and that the windows reconnected.
   If any of these fails, hand back to the previous build at once: `nexus-server handover <previous hash>`.

`~/.nexus/server/config` is read only when the server starts. After you edit it (a `dashboard` line, for example),
run `nexus-server drain` and then `nexus-server handover resume`, which starts the current build again with the
sessions it drained.

To update the desktop client, sign the new `nexus.app` with the same identity as the old one, keep the old app beside
it, and restart the client only. To update an iPhone or iPad, install in place with `tools/ios-device-run`. Never
uninstall the app first: that deletes the device's key.

### Upgrade notes

- **Dashboards.** Dashboards replace the client's `sprint` and `sprint-url` settings. Each dashboard tab is now a
  `dashboard <url> <name>` line in the server's `~/.nexus/server/config`. Remove `sprint-url` from the client's
  `~/.nexus/client/config` and add the server's line in the same deploy, so the tab does not disappear.
- **An applied policy bundle shadows the shipped rules.** If `~/.nexus/client/policy.bundle.json` or
  `~/.nexus/server/policy.bundle.json` came from an older `nexus-policy apply`, the new build runs those older rules in
  place of the ones it ships. Re-apply from the current commit, or move the file aside (keep it) so the host uses the
  rules packaged with its binary. A `tab_labels` entry for the old `sprint` tab kind no longer does anything: a
  dashboard tab's label is the name on its `dashboard` line.
