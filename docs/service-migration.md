# Running Nova under nexus-server

The host starts one nexus-server at boot. The server supervises the long-running programs selected for that machine in nova-config. Friends and tools invoke one-shot commands on demand. macOS and Linux host the server directly. On Windows the host is WSL2. The iPad connects to that server.

## Studio migration

Observed on November 24, 2026. A loaded launchd job can be waiting for its next scheduled run or retry. A registered job alone does not establish that its service is healthy.

| Service | Current owner | What to verify at transfer |
| --- | --- | --- |
| Redis for the message bus | launchd, `bus-redis-v2` | The same persistent data and endpoint survive restart. Existing clients can authenticate and reconnect. |
| Sprint scheduler and repository watcher | launchd, `sprint-run` and `sprint-landwatch` | Preserve scheduling and repository state. Observe one owner for each job. |
| Studio Sprint worker | launchd, `sprint-worker-studio` | Drain active work and confirm the replacement can claim and complete a card once. |
| Scheduled dispatch | launchd, `pingpong-nightly-dispatch` | Preserve the interval and verify a single dispatch per scheduled run. |
| Message bus listeners | Two fleet-managed hook jobs and twelve independently registered hook jobs in launchd. Rowan also has a driver-scoped listener. | Reconcile by bus identity and recipient. Hand off each listener and verify delivery and replies without consuming a message twice. |
| Friend directory | launchd, `com.robotgamedevelopers.nexus-friend-directory` | A registry update reaches connected clients while their overlays are closed. |

The installed nova-fleet service play currently generates launchd or systemd units from nova-config service rows. The transfer needs an explicit owner in that deployment path so the next fleet reconciliation respects it. Preserve each row's arguments, secret seat and key names, schedule, environment, and log destination. Stage and test supervision before stopping the old owner. Switch shared Redis and active workers only with a recovery path to their existing data and jobs.

Bus ingestion, sender identity, and attachment delivery belong on the server. The client renders each incoming peer message with the sender's name and light-blue body text. A body over 4096 bytes belongs in a text attachment accessible in the recipient's workspace.

The fleet migration is complete when an OS restart restores the configured children under nexus-server, closing every client leaves their work running, and restarting nexus-server recovers the same queues and conversations.
