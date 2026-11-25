# Running Nova under nexus-server

The host starts one nexus-server at boot. The server supervises the long-running programs selected for that machine in nova-config. AIs and tools invoke one-shot commands on demand. macOS and Linux host the server directly. On Windows the host is WSL2. The iPad connects to that server.

## Studio migration

Observed on November 24, 2026. A loaded launchd job can be waiting for its next scheduled run or retry. A registered job alone does not establish that its service is healthy.

| Service | Owner before transfer | What to verify at transfer |
| --- | --- | --- |
| Redis for the message bus | launchd, `bus-redis-v2` | The same persistent data and endpoint survive restart. Existing clients can authenticate and reconnect. |
| Sprint scheduler and repository watcher | launchd, `sprint-run` and `sprint-landwatch` | Preserve scheduling and repository state. Observe one owner for each job. |
| Studio Sprint worker | launchd, `sprint-worker-studio` | Drain active work and confirm the replacement can claim and complete a card once. |
| Scheduled dispatch | launchd, `pingpong-nightly-dispatch` | Preserve the interval and verify a single dispatch per scheduled run. |
| Message bus listeners | Two fleet-managed hook jobs and twelve independently registered hook jobs in launchd. Rowan also has a driver-scoped listener. | Reconcile by bus identity and recipient. Hand off each listener and verify delivery and replies without consuming a message twice. |
| AI directory | launchd, `com.robotgamedevelopers.nexus-friend-directory` <!-- the installed service's label, which runs `nova-config friend watch`: the word stays only in that name --> | A registry update reaches connected clients while their overlays are closed. |

Studio now runs Redis, the Sprint scheduler, watcher, worker, scheduled dispatch, AI directory watcher, and its selected inbox hooks under the always-on server. The former host jobs are disabled. The server starts again through the host service manager when it exits.

During the live transfer, a bus message queued before Redis changed owners reached its recipient afterward with the same ID. With the client closed, the Sprint dashboard served its API and the worker registered all eight slots again. The scheduled dispatcher retained its interval. This check did not submit a production Sprint card or reboot Studio.

The installed nova-fleet service play currently generates launchd or systemd units from nova-config service rows. The transfer needs an explicit owner in that deployment path so the next fleet reconciliation respects it. Preserve each row's arguments, secret seat and key names, schedule, environment, and log destination. Stage and test supervision before stopping the old owner. Switch shared Redis and active workers only with a recovery path to their existing data and jobs.

Bus ingestion, sender identity, and attachment delivery belong on the server. The client renders each incoming peer message with the sender's name and light-blue body text. A body over 4096 bytes belongs in a text attachment accessible in the recipient's workspace.

The fleet migration is complete when an OS restart restores the configured children under nexus-server, closing every client leaves their work running, and restarting nexus-server recovers the same queues and conversations.

## Selecting services for this server

The server reads `services.json` from its server directory at startup. An absent file selects zero services. Only rows with an explicit `"owner": "nexus-server"` are eligible. A host-owned, unassigned or disabled row starts no child. Closing clients has no effect on these processes.

```json
{
  "version": 1,
  "services": [
    {
      "name": "sprint-worker",
      "owner": "nexus-server",
      "enabled": true,
      "every": 0,
      "argv": ["/srv/nova/bin/nova-sprint", "worker"],
      "cwd": "/srv/nova",
      "env": {"HOME": "/srv/nova", "PATH": "/srv/nova/bin:/usr/bin:/bin"},
      "log": "/srv/nova/log/sprint-worker.log"
    }
  ]
}
```

Use the actual host's registered command and arguments when transferring a service. The server passes the argument strings directly to the executable. Each child gets the exact environment in `env`. Give `HOME` and each directory in `PATH` as absolute paths. `cwd` and the append-only log destination are absolute. Create the log's parent directory and make it writable by the server user. `every` is an interval in whole seconds. Zero, or an omitted interval, keeps the service running. Positive intervals run one scheduled child at a time. `enabled` defaults to true. A manifest can contain 256 services and up to 1 MiB of UTF-8 JSON. Use a regular file owned by the server login and writable only by that login.

Sprint is enabled by default after setup. Give each Sprint scheduler, worker, and associated scheduled job `"feature": "sprint"` in its service row. The projection tool tags a direct `nova-sprint` executable automatically. Pass `--sprint-service NAME` for a wrapper. To stop Sprint on this server, set the top-level `"sprint_enabled": false` and reload the manifest. Set it to true and reload to resume the individually enabled services. Other services, including the bus, continue running. The dashboard tabs are nexus-server's: one for each `dashboard <url> <name>` line in `~/.nexus/server/config`. To show none, remove those lines and restart nexus-server.

`nexus-server services check` validates the file on disk. `nexus-server services reload` asks the running server to replace its configuration. An invalid replacement leaves the active set in place. `nexus-server services status` reports the server's current service state. Each command accepts `--dir DIR` to use another server directory.

Run each service in the foreground. Nexus supervises the executable and its process group. A status of `running` means the executable started. Verify application readiness through that service's own health check. If a keepalive process exits, Nexus retries after one second and increases the delay up to 30 seconds for repeated failures. A removed or replaced service receives SIGTERM. Nexus gives the group time to exit and uses SIGKILL if it still needs to end.

For a scheduled service, the initial run is due one interval after selection. Nexus records each claim on disk before starting the command. A restart preserves the claim and a long run finishes before another interval starts. A host failure between the durable claim and execution can skip that occurrence. Nexus never replays missed intervals in a burst. Each service has its own lease under `service-leases/` in the server directory. If a helper is killed and leaves a group behind, Nexus reports the group ID and waits for it to exit. Verify the group belongs to the named service before retiring it through its own administration procedure.

### Transferring a fleet service

`nova-config inventory --list` includes each host's service commands, environment, schedules, secret seat and granted key names in `_meta.hostvars[HOST].nova_services`. The fleet play currently treats an enabled row as its own. During a planned transfer, first drain the service, keep a copy of its settings, and disable that row through its existing fleet owner. Apply the services play and verify the old job has stopped. For each independently installed bus hook, also retire its previous supervisor and check its recipient identity. Keep the host row disabled for as long as Nexus owns it.

The offline helper projects an explicitly selected set from a saved inventory. Supply the service login's home on the target host. The helper rejects enabled host rows and writes only the new JSON to stdout. For example, after retiring `sprint-run` and `sprint-worker-studio` through their actual owner, export the host's current inventory and prepare the complete Nexus-owned set.

```sh
nova-config inventory --list > inventory.json
tools/nexus-service-manifest --host studio --home /srv/nova \
  --take sprint-run --take sprint-worker-studio < inventory.json > services.next.json
```

Use the host's actual home in place of `/srv/nova`. Include every service that this server is to own in the replacement. The projection preserves literal arguments, including strings tagged by Ansible as unsafe for template interpolation. It honors row-level log and environment overrides. Rows with keys run through the same seat-scoped `nova-secrets exec --only` invocation as the fleet play. Only key names and paths appear in the projected command. For nondefault target layouts, pass the corresponding `--bin-dir`, `--path`, `--store`, `--key-dir` and `--sops` options.

Validate the projection in a staging directory as that login before putting it in the real server directory.

```sh
stage=$(mktemp -d)
install -m 600 services.next.json "$stage/services.json"
nexus-server services check --dir "$stage"
```

After checking the whole replacement, atomically install it as the server's `services.json`, issue `services reload`, and check `services status` and the service's own health. For rollback, remove the service from the Nexus manifest, reload, confirm the owned process has stopped, then restore the old owner's row and unit. Sprint's `run` process starts its own Sprint bus hook. Give that identity a single owner. Redis provisioned by the dedicated fleet role needs its existing executable, config, data directory and a separately verified handoff. Each nonresident Nova CLI remains available to server-side programs on their configured PATH.

For a Nexus AI's incoming chat messages, transfer its inbox using the
[message bus ownership settings](accounts.md#message-bus-ownership).
The native receiver starts the hook for that inbox and turns its deliveries into chat bubbles.
Remove the old standalone hook for that identity at handoff so only one listener owns it.

A Claude session with a server-owned inbox can send through the in-process `nexus_bus_send` tool. Pass `to`, `subject`, `body` and optionally `reply_to`. The driver fixes the sender and endpoint from the AI grant and returns the bus receipt.
