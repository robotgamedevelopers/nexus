# Formats across versions

A window and `nexus-server` never have to be the same version, and a server reads every store an older one left.
This page is the whole scheme. The code is `src/sync/formats.h` (the numbers, the Hello, the refusals),
`src/sync/sessions.cpp` and `src/sync/actions.cpp` (what a diff says, the same in every format), `src/sync/codec.h`
(the codecs: `tables_codec.cpp` for format 4, `legacy_codec.cpp` for 1 to 3), `src/link/bootstrap.h` and
`src/server/server.cpp` (the agreement on connect), and `src/store/store.cpp` (the store).

## The rule

1. Every format from the first framed one on frames each record that can grow (a session, an entry; in the store a
   change, an entry, a session) by its length. A later version appends its new fields at the end of a frame, or after
   the last field of the whole, and a reader skips what it does not know. It never moves or reinterprets a field.
2. A field an older writer did not write reads as a default. The default is the one that claims least, stated below
   with why.
3. A value a reader does not know (an entry kind, an activity) reads as the plainest thing it does know, never as an
   error.
4. A writer can declare that an older reader must refuse it: the state by its Hello's `min_peer`, the store by the
   oldest format a reader must know (a byte after the format). Nothing is declared incompatible today.

Each installed build of the server records the formats it writes and reads in its `formats` file, and a build that
cannot read what the current one writes is refused as current: `docs/builds.md`, "The formats rule".

## The sessions state (server to window)

| Format | What it is |
| --- | --- |
| 1 | The first (main before formats): nothing framed, no activity, entry kinds Person 0, AI 1, Notice 2. A reader of 1 reads exactly 1. |
| 2 | Each session and each entry framed by its length (`Writer::str`); the session's activity after its effort; entry kind Tool 3. Anything after the sessions, and after the known fields of a frame, is skipped. Appended since, at the end of the session's frame: its instance (`str`). |
| 3 | Format 2, then the open secret requests after the sessions (`docs/secrets.md`): a count, then each request framed by its length (its id, account, what is asked, the asker's session, the asker's AI, the asker's reason). The list is always written (count 0 for none), so a diff cut short before it is refused. Anything after a request's known fields, and after the list, is skipped. The actions up are framed from 3, with the secret dialog's answer (15) and cancel (16). A window of 2 receives no requests, and the server counts it as unable to take one; a secret action, which 2 cannot carry, is written as a stop naming session 0, refused by every server. |
| 4 | schema's variable tables (`schema/Nexus.schema`): the state is a `SessionsDiff` and the actions an `ActionsDiff`, written and read by the generated code. Every field, enum value and union arm rides under the hash of its name, so rules 1 to 3 hold by construction: a field a reader does not know is skipped, an enum value it does not know reads None, and both are counted (`TableReport.unknown`), which the window logs when the count changes, for the state it receives. The server logs no such count: mosh hands it the actions re-encoded from those it kept, where an unknown field is already gone, and it refuses an unknown action kind with its reason instead. None reads as the plainest thing known: an entry kind as a Notice, an activity as Busy, an action kind as unknown (which the server refuses alone, never the link). It says the same as format 3. |
| 5 | Format 4's tables with the in-app sign-in added (`docs/secrets.md` "Sign-in"): `SessionsDiff.signins`, the live sign-in flows as each window sees them (`SignInOffer`: its id, account, provider, the claim token of the window holding it or `*` to every other window, the server's host name, and its URL, set only in the holder's state), and the action kinds `SignInClaim`, `SignInOpened` and `SignInCallback`. A format 4 reader reads through all of it (rule 1: the field skipped and counted, the kinds read as unknown and refused alone), but a format 4 window would never act on a flow, so the server must know which windows do: that is why this is a new number rather than an added field. The server reads whether a window acts on flows at all from the window's Hello (the link's format, the older of the two protos) and nothing else; whether a window can take a pasted code, a later addition within format 5, it reads from that window's claim (`Action.pastes_code`, below). It writes `signins` only on a link of 5 or later, takes the three kinds only from one, and while no linked window is of 5 or later it asks for a claude account's login by a paste instead (`docs/secrets.md` "Versions"). The typed `SignOut` action adds an account name and session check on links of format 5 or later. Earlier servers on format 5 count the unfamiliar action as unknown. The hand-written codecs of 1 to 3 carry no sign-in. |

An account switch is a durable Notice whose text includes the destination address and label as they were when
the switch succeeded. `EntryChange.account_email_end` and `StoreEntry.account_email_end` mark the byte just
after the email in `Switched to <email> (<label>)`. When no email is configured, the notice reads `Switched to <label>`
and the offset equals the prefix length. A zero or absent offset means an ordinary notice. Earlier readers show
the complete notice in their usual text color.

Fields appended to a format's frames do not change its number: a reader from before them skips them (rule 1), and
nothing about them makes an older reader wrong. They are listed in the table in the order they were appended, and a
later field goes after them.

- Absent activity (a format 1 server): **Idle**. Idle claims least: it says nothing is running and nothing is wrong,
  and a send is always safe, since the server answers a send it cannot carry out with a notice in the session.
- Unknown entry kind: a **Notice**, a plain dim line with the entry's text. The text is never lost, and a notice
  claims nothing about who wrote it.
- Unknown activity: **Busy** (the pulsing light blue). Something a later server knows is going on; busy says work
  may be under way without claiming the AI is waiting (idle) or broken (down). A stop is always allowed.
- A server records an incoming bus message as framed store entry kind 5 and sends it as `EntryKind.Bus` on a
  table link. Its text starts with the bus sender’s name and one newline, followed by the display body. Newlines
  in the body are preserved. Windows use the entry kind to identify bus traffic. An older table reader renders
  the text as a Notice. On links of formats 1 to 3 the server sends the text as a Notice directly.
  A bus entry's store frame can append the original driver prompt as a length-prefixed string. The server uses it
  when rebuilding context to retain a peer's attachment path and to exclude prompts still waiting for their turn.
  It stays in the store. An older frame with no tail still provides the sender and display body.
  Before encoding the tail, the server checks each stored bus sender against the recipient's current
  Nova profile. An unregistered sender's position carries a closed Bus entry with empty text, including
  when an older codec writes that position as a Notice. This preserves transcript indices across roster
  changes. A table link also carries `SessionChange.bus_senders_known` and `bus_senders`, the sorted
  registered names that have spoken in this session's stored Bus entries. An explicit empty list hides
  every bus row. A missing field identifies an older writer. A current window uses these names to update
  bubbles it has already displayed, including entries preceding the bounded tail. A reconnecting window
  can recover message bodies from the current tail, plus any earlier history the window already received.
- A format 1 link carries a Tool entry as an AI entry (the line's text unchanged) and no activity.
- Absent secret requests (a link of format 2 or older): **none**. A window with none shows no dialog and sends no secret
  action; the server, which knows the link's format, never offers that window a request.
- Absent sign-in flows (a link of format 4 or older): **none**. The window claims, opens and binds nothing; the server,
  which knows the link's format, asks for a paste while no linked window reads flows.
- A pending model-limit choice adds `limit_request`, `limit_account`, and `limit_model` to the session info table.
  An absent or zero request means no choice. An empty target makes that option unavailable. The `ResolveLimit` action
  binds the session, interrupted request, current payer, choice kind, and offered target. An earlier table reader
  skips the fields and refuses the unknown action. The legacy codecs never offer this choice.
- A sign-in by a pasted code (Antigravity) adds `SignInOffer.code_wanted` (bool, default false: no code is asked for),
  set only in the state of the window holding the flow; the action kind `SignInCode`, whose `secret` carries the code
  and whose `text` the window's claim token; and `Action.pastes_code` (bool, default false: the window cannot take a
  code), which a window that takes a pasted code sets on its `SignInClaim`. None raises the format (rules 1 and 2): the
  server learns what it needs from the claim itself, when it matters, so a flag on the claim is enough where format 5
  needed a new number (that server had to know, before any claim, which windows read flows at all). Each mix of
  versions ends in a plain line, never a silent wait for the flow's deadline:
  - An earlier window on this server: its claim of an Antigravity flow reads `pastes_code` false, and the server
    refuses it with a line in that session ("... signs in with a code pasted into Nexus, which this window cannot
    take. Update this Nexus window, ..."); the flow stays needed, for an updated window to claim.
  - This window on an earlier server: that server still offers its private loopback page for an Antigravity flow. A
    window on the server's machine opens it as before; one on another machine opens nothing and says "nexus-server is
    too old to sign in to <account> from another machine", and that a window on the server's machine can press Sign
    in once this attempt ends (its claim holds the flow until the deadline, within 10 minutes).
  - An earlier server would read `SignInCode` as unknown and refuse it alone; the window never sends one there, since
    it asks for a code only when `code_wanted` is set.

  The legacy codecs carry none of it. `nexus_versions_test` `paste` reads this build's bytes with main's format 5
  reader (`tests/wire/main5`, the schema as it shipped before these fields) and main's bytes with this build's.
- Absent instance (a server from before instances, or a format 1 link): **none**, empty. The window groups the session
  by its AI alone; the server, which holds the AI's instances, settles an empty instance to the AI's
  default instance (its first) and says so in a notice, so an empty one never decides where a driver runs.
- Absent list of AIs (a server from before it, or a link of format 3 or older): **not known**. Format 4's
  `SessionsDiff` carries `ais_sent` (bool, default false), `ais_known` (bool, default false) and `ais` (a
  list of `AI` records with a `name` and linked `accounts`; default empty), the AIs the server's `ais.json` holds, in its order. An
  AI's accounts are ordered `AccountChoice` records with the stable `name` used for SetAccount and an optional
  display `label`. The optional `email` is the address from the account’s server registry entry. It reads as
  empty for older servers. Older windows skip it and retain the account name, label and sign-in status.
  The optional `needs_sign_in` flag defaults to false. The server sets it for a linked account
  when its browser sign-in is available and the server has no active login for it. A window can send a
  `SignInClaim` naming the session and account the person selected, its claim token and an existing flow ID,
  or zero to request the first flow. The server verifies the session still uses that linked account before
  claiming its flow. With a named client, `SessionsDiff.next_action` is the first number from that client
  the server had yet to apply when it built this snapshot. Zero means unavailable. The window waits for a
  snapshot past its account-selection action before it can submit the draft or start a sign-in for that session.
  A window with advertised choices can send SetAccount text prefixed with `+` to invoke a named
  account command in the server profile. Every account ID excludes `+`. The server resolves the command against
  the current session and validates its resulting ID before applying the ordinary SetAccount operation. Older
  servers omit the choices, which read as an empty list, and the window sends them no account commands. A
  diff carries the AIs, with `ais_sent` true, only when they differ from the state the diff is from (the one
  the reader holds); without `ais_sent` the reader keeps the AIs it holds, so an unchanged list costs no bytes.
  A first diff, from the empty state, carries them whenever the server has a list. A diff from a server before these
  fields has no `ais_sent`, and the reader holds no list: not known. The window builds its AI tabs from the
  list when it is known, adding and removing tabs as it changes; when not known it takes the AIs of the sessions it
  is sent, adds a tab for each new one and removes none, as a window did before the list. Formats 1 to 3 carry none,
  byte for byte as before (`sync_unit` `ais_cross_the_wire`, `ais_not_in_older_formats`,
  `ais_sent_only_when_changed`).
- Absent dashboards (a server from before them, or a link of format 3 or older): **none**.
  `SessionsDiff.dashboards` (`[]Dashboard`, each a `name` and a `url`, both `*string`) is the list of dashboards
  the server offers, from its config's `dashboard <url> <name>` lines in their order (`~/.nexus/server/config`,
  `server/home.h`; none by default), whole in every diff (`sync/dashboards.h` holds the bounds: at most 16, a name of 1
  to 64 bytes, an `http://` or `https://` address of at most 2048 bytes, no name twice). A window today shows the
  first at its dashboard tab (`board.h`); a tab for each is the next step (docs/ios.md); with none, the window finds
  its board as before. An older window skips the field. Formats 1 to 3 carry none (`sync_unit`
  `sessions_dashboards_appended`).

Text in format 4 is schema's text: well-formed UTF-8 with no NUL, or the whole diff would not load. The server writes
what it holds with each malformed byte, and each NUL, shown as U+FFFD (the replacement character), so an AI's
stray byte costs one character on screen, never the link. An entry whose text is not clean is sent whole each time,
never as growth, so the window's copy is always the server's text as shown; so is an entry whose earlier text the
window holds was not clean (cut inside a character the growth completes), since the window holds that text with
U+FFFD and a growth from the server's length would not be where the window's is. A secret's value rides as bytes,
never as text, and is never changed.

A format 4 read is bounded. A diff longer than 4,194,304 bytes (2048 * 2048, mosh's `Compressor::BUFFER_SIZE`, the
most one instruction of the link decompresses into) never crossed the link and is refused before it is measured; and
a diff whose loaded tables would take more than 16,777,216 bytes (four times that) is refused before anything is
allocated (`sync/codec.h`: `kMaxDiffBytes`, `kMaxReadBytes`). A real diff loads into 1.05 to 2.1 times its bytes;
bytes crafted as many tiny elements load into ten times or more, and those are what the second cap refuses.

### Agreeing on a format when the window connects: the Hello

A Hello (`schema/Nexus.schema`) is two numbers: `proto`, the protocol an end speaks (its state format, 5 today), and
`min_peer`, the oldest protocol of the other end it still works with (1 today). It travels as lowercase hex of its
table bytes, so a later version can add to it like any table.

1. The window runs `env NEXUS_STATE_FORMAT=<its format> NEXUS_HELLO=<its Hello> nexus-server new ...` over ssh. A
   server before formats ignores both variables; one before Hello ignores `NEXUS_HELLO`. A Hello that does not read
   is said on standard error and left out, and the server goes by the format alone.
2. `new` asks the server `new[ <client> <next>] format <n>[ hello <Hello>]`. A server that does not know a word answers
   `ERROR unknown request; ...`, and `new` asks again without the Hello, then without the format.
3. The server refuses only when the window's `proto` is below the server's `min_peer`, or the server's `proto` is below
   the window's `min_peer`. Otherwise it answers `NEXUS CONNECT <port> <key> format <m>[ hello <its Hello>]`, where `m`
   is the older of the two protos: versions that differ work together. It adds `format <m>` only when the window
   stated one, and its Hello only when the window stated one. With no `format` in the answer, the link is format 1.
4. The window decides the same on the server's Hello (a server from before Hello: its format, working with every
   older window). A refusal is one plain line in the window, naming which end is too old and what to do.
5. Both ends then read and write the state and the actions in `m`.

A window or server from before Hello states only its format; the other end reads that as a Hello of that proto with
`min_peer` 1, which is what it was.

The actions (window to server) are unframed in formats 1 and 2 and framed from 3, under the same link format number.

### State files

`nexus --state FILE` and the tests' fixtures keep a state as the line `nexus-state <format>`, then the state's
`init_diff` in that format (`sync::stateFile`, `sync::readStateFile`). A file of a later format is read as a later
server's state is.

## The store (`src/store/store.h`)

| Kind | Format | Reader byte | What it is |
| --- | --- | --- | --- |
| record | 1 | | an AI tab's, by name: migrated at open |
| record | 2 | | by session, unframed, entry kinds 0 to 2 |
| record | 3 | 3 | by session; each change framed by its length, each entry framed in it; trailing fields skipped. Appended since: an info's frame carries the instance (`str`) and optional Auto marker (`0xA2`), a driver's with the summary flag (`u8`) and its session's directory (`str`), followed by the optional session account (`0xA1`, `str`) |
| snapshot | 1, 2 | | AI tabs (1 before drivers): migrated at open |
| snapshot | 3 | | by session, unframed, entry kinds 0 to 2 |
| snapshot | 4 | 4 | by session; each session framed, each entry framed in it; trailing fields skipped. Appended since, after the driver: the summary flag, its session's directory, then the instance, then the optional session account (`0xA1`, `str`) and optional Auto marker (`0xA2`) |

- The Auto marker records automatic subscription selection. Its absence preserves the stored named selection.
- Absent instance: **the AI's default instance**, settled at the server's start with a notice, as above.
- An interrupted request may append a continuation to the driver record or session snapshot. Tag `0xA3` introduces the original prompt (`str`), the exhausted account names (`u32` count, then each `str`), and a model-limit decision pending (`u8`). No tag means no outstanding continuation. Its transcript effects and turn boundary commit together. An open turn on recovery may already have executed, so the server cuts it. A checkpoint between turns can safely continue.
- Absent summary flag: **none owed**. An absent session account means the owner of the harness ID is unknown.
  The server starts a new harness session and provides recent transcript entries on its first send. An explicitly
  empty account identifies the accountless harness. A known account may resume in the same working directory,
  or when an earlier format left the directory unknown.
- An older server that rewrites the store (a compaction) drops the appended fields; a newer one then reads their
  defaults.

- A newer server reads every older format. Format 2 records and format 3 snapshots are read as they are, never
  rewritten. An AI-tab store is compacted into the current format at open.
- An unframed format holding an entry kind it never had is refused, naming the store, the record (or snapshot) and
  the kind. That is damage, not a later version.
- A framed record or snapshot of a later format is read. A change of a kind this build does not know is skipped
  whole (including a record on a held session whose only changes are unknown kinds, still advancing any client counter
  on that record); fields after the known ones are skipped (`OpenReport::skipped` counts skipped frames and trailing
  tails, logged once on `serve` and shown by `nexus-server store check`); an entry kind it does not know is kept as it
  is, and the server shows it as a notice (`server/durable.cpp`).
- It is refused only when its reader byte names a format later than this build reads. The message names the store,
  the record and both formats, and says to run the newer nexus-server. `Store::inspect_readers` (`nexus-server store
  check --for <hash>`, `builds back`, and write-downgrading `builds current`) scans the snapshot's and journal records'
  reader bytes under the store's shared lock without mutating anything on disk (`models/migration/Migration.tla`).
- A server layout from before `~/.nexus/server` existed (`~/.nexus/{store,drivers,config,server.log}` and a stale
  `~/.nexus/server.sock`) moves once into `~/.nexus/server` at startup (`src/server/migration.cpp`), under
  `~/.nexus/server`'s exclusive `flock`: refusing while an old server answers on `~/.nexus/server.sock` or holds
  `~/.nexus/store`, refusing rather than overwriting a populated `~/.nexus/server/store` or differing `drivers`/`config`,
  and recording `.migrating` synced before copying so any kill or crash resumes cleanly without losing or tearing a file.

### One writer: the store's lock

The journal has one writer. Two writers would each number their next record from the same replay, so one number
lands twice and the next open keeps only what comes before it, and each could start a driver for the same queued
prompt. So at most one process ever has a store open for writing, and the operating system enforces it
(`models/store/StoreLock.tla`):

- The lock is the store's directory's own: an advisory lock (`flock`) on a descriptor of the directory itself
  (`O_RDONLY | O_DIRECTORY | O_CLOEXEC`). Nothing is made for it: no lock file, no pid. A server takes it exclusive as
  the first thing `Store::open` does, before anything in the directory is read or written, and holds it for the life
  of the process. The operating system releases it when the process ends, however it ends (kill -9, a crash, an exit),
  so there is never a stale lock to clean up. The descriptor is close-on-exec: no driver or other child holds it.
- The directory is opened by its name once, following symbolic links, so every name it is reached by (a second server
  directory whose `store` is a link to this one, a path through a linked parent) takes the one lock. From then on the
  store is reached only through that descriptor: every open, read, rename, removal and directory sync is relative to
  it (`openat`, `renameat`, `unlinkat`, `fstatat`). A directory moved aside or renamed while its server holds it is
  still that server's store: its records and its compactions go on in the moved directory, never into a new directory
  made at the old name, which is another store with its own lock.
- The lock is checked where the store lives: right after it is taken, a second exclusive lock on another descriptor
  of the same directory must be refused. A filesystem that grants it does not keep locks, whatever its type, and the
  store is refused.
- Where locks cannot be trusted the store is refused, never run unlocked: a network filesystem (on macOS any volume
  without `MNT_LOCAL`; on Linux NFS, SMB, CIFS, AFS, Ceph, GPFS, Lustre, BeeGFS, Coda and NCP), and a filesystem
  that hands locks to another program (FUSE, virtiofs, which reports FUSE's type, 9p, which is WSL2's view of Windows
  drives, and VirtualBox's shared folders). The message names the filesystem (`DIR is on NFS, a network filesystem,
  ...`) and says to keep the server's directory on a local disk. A filesystem that returns an error for `flock` is
  refused the same way.
- A reader that changes nothing (`nexus-server store check`, `Store::inspect`) takes the lock shared, copies the
  journal's files into memory and lets go; it opens its copy in memory, never cuts a torn tail or writes a migration on
  the disk, and makes nothing in the directory. While a server holds the lock it is refused in one line and reads
  nothing. Its copy has a hard limit, 500 ms under the lock, checked before every 256 KiB it reads and after every
  file, so one slow read of a large file cannot outlast it by more than one chunk: past it the reader gives up and
  fails (the reader, never the writer).
- Writers come before readers, by a gate: the file `gate` in the store's directory (empty; made by the first server of
  this build that opens the store, opened through the directory's descriptor, `O_NOFOLLOW`). A server holds it
  exclusive while it waits for the store's lock; a reader takes it shared, without waiting, only for the moment it
  takes the store's lock, and when a server holds it gives way at once, in one line (`has a nexus-server starting on
  it ... try again once the server serves`). So while a server waits, no new reader starts, and it waits only for the
  readers already in, each of which gives up within its 500 ms. A second server finding the gate held finds a writer
  opening the store: it waits as for a held lock (step 2 below).
- A server that starts while only readers hold the lock tries again every 10 ms for at most 2 seconds; if they have not
  let go by then it fails (exit 1), saying that readers hold the store and how to find them (`lsof DIR`). What stays
  unbounded: a reader that is suspended (stopped with a signal, its machine asleep, a debugger) holds its shared lock
  for as long as it is suspended, past its 500 ms; every server start in that time fails after its 2 seconds, and a
  service manager's restarts may reach its start limit (below).
- A store's directory removed while its server holds it (`rm -rf`, by hand) is found at the next commit (once per
  group commit, before the journal's sync): on Linux by the directory's link count, which is 0 once it is removed; on
  macOS, where a removed directory keeps its count, by its present path (`F_GETPATH`, which follows a rename), which no
  longer names that directory. The store fails, loudly (`... was removed while this server held it`), nothing more is
  acknowledged, and the server stops (exit 1); a directory moved aside is not removed, and its server goes on in it.

`nexus-server serve` starts in this order (`src/server/start_order.h`, the one dispatch `take_step`):

1. Ask the control socket whether a server is serving: the request `status`, and the reply `SERVING`, within 3 s (past
   the 2 s one idle connection may hold a serving server in its read). A server answers it only from its serving loop,
   so `SERVING` is true when said; a server that has accepted `stop` takes no more requests, and the connections still
   queued on its socket are closed unanswered with it. A connection alone is not an answer: the kernel accepts one on
   any bound socket, a stopping server's too. A server from before this question (which takes no store lock either)
   refuses `status` as it refuses every request it does not know, `ERROR unknown request ...`, and gives that refusal
   only from its serving loop: that reply counts as serving, and no other does. Serving: one line, exit 0. No reply,
   the connection closed unanswered, `STOPPING` or any other line: not serving, and the start goes on.
2. Take the store's lock and replay the journal. The lock held by a writer that does not say it serves is a server
   starting (its replay) or stopping (from its stop's acceptance to its drivers' end): wait, asking the socket every
   10 ms, and saying so once a second (`still waiting for the store's lock: N of 10000 ms`). A server says it serves:
   exit 0. The lock comes free: take it and go on. Neither within 10 s: exit 3, saying that the lock's holder is
   neither serving nor ending and how to find it. Only readers holding it past their wait: exit 1.
3. Bind the control socket (a server says it serves on it: exit 0), recording the socket file it made (its device and
   inode).
4. Only then settle the sessions against the accounts and restore the AIs' drivers, which may commit to the
   journal and start a driver for a queued prompt.

Nothing writes a record or starts a driver before the lock and the socket are both held. Exit 0 is a start refused
because a live server says it serves, which a service manager does not restart (launchd's `KeepAlive` with
`SuccessfulExit` false, systemd's `Restart=on-failure`); exit 3 (stuck) and exit 1 (anything else, readers that did
not let go among them) are restarted. A start that fails after it bound removes its own socket file.

`nexus-server new` waits on the same outcomes the start reports, not a fixed time: a reply from a serving server; or
the exit of the server it started: 0 (another says it serves: asked again), anything else said as it is, with the log
(`the server started exited with code 3 without serving; see DIR/server.log`). A request closed unanswered (a server
that has accepted `stop`) is asked again, and once its socket is gone a server is started, which waits for the lock. A
last-resort bound of 30 s, past the started server's whole wait, ends it only for a server neither serving nor exiting.

For the supervision layer (a service manager running `nexus-server serve`):

- What the start's liveness rests on (`models/store/README.md`, fairness): every non-zero exit is started again, for
  as long as it takes. systemd's default start limit (`StartLimitBurst=5` in `StartLimitIntervalSec=10s`) breaks
  that: five quick exits 1 (readers that did not let go, each start failing after its 2 s; a store that does not
  read) put the unit in `failed` and nothing starts it again. A unit for nexus-server sets `StartLimitIntervalSec=0`,
  or a `RestartSec=` long enough that five restarts span more than the interval (`RestartSec=3s`). launchd has no
  start limit; it throttles a job that exits within 10 s of starting to one start per 10 s (`ThrottleInterval`).
- A person who starts the server by hand is no service manager: nothing starts it again after exit 1 or 3. The line it
  exits with says what held the store and how to find it.
- A start waiting for the store's lock says so once, then once a second until it serves or gives up, so a person
  watching it sees the wait go on.

A server stops in this order: `stop` (or SIGTERM, SIGINT) accepted, after which its loop takes no request; the control
socket closed and its file removed, only if the file of that name in the server's directory (held by descriptor) is
still the socket this server bound (same device and inode); the drivers ended and their last records written, under
the lock still held; then the process exits and the lock goes with it. Between the stop's acceptance and the socket's
close a start can still connect, but it is not told `SERVING`, so it never takes a stopping server for a serving one:
it finds the lock held and waits for it (step 2) rather than exiting 0 and leaving nothing serving
(`models/store/StoreLock.tla` RefusedServes; its reversed witness `witnesses/LConnectProbe.cfg` is the probe that took
a connection for an answer).

The server's directory is held by descriptor too, from the start: the socket is bound in it (on Linux through the
descriptor's `/proc/self/fd` path; on macOS, which has no bind relative to a directory, by changing into the directory
for the call), and read back, changed and removed in it (`fstatat`, `fchmodat`, `unlinkat`); the config repository
and the drivers file are found through it each time (its present path), so a directory moved aside while its server
runs keeps everything that server does in one directory. What stays open: between the bind and the read-back, and
between the identity check and the removal at the stop, another process of the same user could put a file of its own
at the socket's name, inside the server's own directory (mode 0700); there is no call that removes a name only if it is
a given file. The start's first question goes to the socket by its path, as `new` and `stop` do.

What the lock covers: any number of servers and readers of this build on one store, by any name of its directory, the
directory moved or renamed under its server, any of them killed at any point, and filesystems that do not keep locks
(by type, or by the check). What it does not cover:

- A server from before the lock takes none; only the socket keeps it out, and that leaves one window, open during the
  upgrade only (while servers of both builds can start on one store): a server of this build, holding the lock, asks
  the socket again before it binds, and removes a stale socket file before binding its own. A server from before the
  lock that binds between that second ask and that removal has its socket file removed and keeps running unreachable,
  beside this one. Such a server also opens the store and restores its drivers before it asks the socket at all, so
  one started while a server of this build replays writes to the journal unlocked before it is refused. And the
  commonest path of the upgrade itself: the older server is stopped, and its socket goes first while its drivers end,
  with no lock on the store; a server of this build started then finds no socket and a free lock, takes it, and
  replays while the older one's drivers still write their last records. Nothing is built around any of these: stop
  the older server and wait for it to exit before starting this build (`nexus-server stop`, then its process gone);
  once every server on the machine is of this build, the lock closes all three.
- A process that writes the store's files without `Store::open` (by hand, or a tool reading the journal directly).
- A store shared with another machine in a way no filesystem type shows and the check passes on (a sync tool copying
  the directory, one disk mounted by two machines).
- A copy of the directory: it is another store, with its own lock.

## The fixtures

Today's main formats are fixed as bytes that main's own code wrote, not as bytes this build claims main would write:

- `tests/sync/fixtures/main-state-1.init` and `.grow`, from `tests/sync/fixtures/main_state_fixture.cpp`;
- `tests/store/fixtures/main_store.h`, from `tests/store/fixtures/main_store_fixture.cpp`.

Each generator says how to build it against a worktree of main at 50565c6. `sync_unit` (`state_format_1_from_main`,
`state_format_2_reads_later_formats`, `state_files`, `link_formats_agree`), `store_unit_formats`,
`durable_unit_kinds` and `link_e2e_older_server` check the rules above; `sync_unit` `sessions_instance_appended` and
`store_unit_instance` check the appended fields both ways. `wire_unit_nexus_sessions`, `wire_unit_nexus_actions` and
`wire_unit_nexus_hello` check format 4 and the Hello both ways against a later version; `wire_unit_nexus_signin`
checks format 5 both ways against format 4 as it shipped (`tests/wire/prev/NexusPrev.schema`, frozen): the link of 4
from the two Hellos, on which no window acts on a sign-in; the flows' field skipped and counted by a format 4 window;
the sign-in kinds read as None by a format 4 server, and written on a link of 4 as a stop naming session 0; and
`signin_unit_hub_older` the paste asked for while only such windows are linked; `sync_unit` `hello_min_peer`,
`sessions_unknown_kind_is_a_notice`, `sessions_text_not_utf8` and `sessions_growth_across_a_split_character` check
the refusal, the unknown kind and the text rule; `format4_read_caps` the two caps at their boundaries; and
`format4_read_fuzz` thousands of damaged diffs (fixed seeds; from format 5 the sessions diffs carry sign-in flows, a
URL among them, and the actions every kind, a sign-in callback's query among them), each read taken or refused as BadDiff alone, a refused
one leaving the state as it was, a taken one writing back out the same.

## Moving to schema

These formats are moving onto schema's variable tables (github.com/mas-bandwidth/schema), where every field, enum
value and union arm rides under the hash of its name, so that rules 1 to 3 above hold by construction rather than by
each codec's care. Step 1, the plumbing, is in: `schema/Nexus.schema` mirrors every field above (with the accounts work's
instance and account fields), its generated C++ is checked in under `schema/gen` (the library `nexus_wire`), the
compiler is pinned to one exact commit in `cmake/schema.cmake` (its table wire is pre-release until schema 3.0.0), and
`schema/tables.baseline` refuses an edit that would change what stored bytes mean. CI regenerates the code at the pin
and fails on any difference (`schema-generated`). `wire_unit_versions` proves the requirement: a later version that adds
a field, an enum value and a union arm (`tests/wire/next/NexusNext.schema`) and this one read each other's bytes both
ways. Step 2 is in: the state and the actions are on the tables from format 4 (the hand-written codecs stay for links
of 1 to 3, so an older end still works), and the Hello replaces the old floors. The store is next; its record and
snapshot formats above are unchanged.

A rename is `| was = "<first name>"` (schema's §5): the id stays the hash of the first name, so the bytes do not move
and every reader, older or newer, reads the renamed field as before. When nexus called its AIs "AI" (2026-11-25),
`friend_name`, `friends`, `friends_known`, `friends_sent`, the table `Friend` and the values `Friend` and `AddFriend`
became `ai_name`, `ais`, `ais_known`, `ais_sent`, `AI` and `AddAI` this way; `schema/tables.baseline` records each
first name, and `wire_unit_versions` and the fixtures of main's formats read them both ways unchanged.

Encode and decode on the link thread, never the window's frame thread (`tests/sync/wire_bench`, release build, mean of
one): a typical frame's diff (one open entry grown by 120 bytes, 404 bytes) 0.023 ms to encode and 0.002 ms to
decode; a first state of sixteen sessions of sixteen 1 KiB entries (267 KB) 0.18 ms and 0.26 ms; eight 200-byte sends
up 0.023 ms and 0.003 ms. Format 3 is faster per message (0.001 ms for the typical diff), and every one is far inside
a 120 Hz frame's 8.3 ms.


A session may advertise a `catalog` containing the model and effort choices reported by its
harness. An empty catalog means discovery has supplied no choices. `context_epoch` advances
on a server-side clear, so windows discard entries from the previous context even after a
reconnect. The durable journal records this boundary as framed entry kind 4. Snapshots preserve
boundaries. The active transcript and context handoff use entries after the last boundary.
