# Formats across versions

A window and `nexus-server` never have to be the same version, and a server reads every store an older one left.
This page is the whole scheme, kept small so that moving it to a schema tool later is mechanical. The code is
`src/sync/formats.h` (the numbers, the floors, the refusals), `src/sync/sessions.cpp` (the state's codec),
`src/link/bootstrap.h` and `src/server/server.cpp` (the agreement on connect), and `src/store/store.cpp` (the store).

## The rule

1. Every format from the first framed one on frames each record that can grow (a session, an entry; in the store a
   change, an entry, a session) by its length. A later version appends its new fields at the end of a frame, or after
   the last field of the whole, and a reader skips what it does not know. It never moves or reinterprets a field.
2. A field an older writer did not write reads as a default. The default is the one that claims least, stated below
   with why.
3. A value a reader does not know (an entry kind, an activity) reads as the plainest thing it does know, never as an
   error.
4. A writer can declare that an older reader must refuse it: the state by its link's floors, the store by the oldest
   format a reader must know (a byte after the format). Nothing is declared incompatible today.

## The sessions state (server to window)

| Format | What it is |
| --- | --- |
| 1 | The first (main before formats): nothing framed, no activity, entry kinds Person 0, Friend 1, Notice 2. A reader of 1 reads exactly 1. |
| 2 | Each session and each entry framed by its length (`Writer::str`); the session's activity after its effort; entry kind Tool 3. Anything after the sessions, and after the known fields of a frame, is skipped. Appended since, at the end of the session's frame: its instance (`str`). |

Fields appended to a format's frames do not change its number: a reader from before them skips them (rule 1), and
nothing about them makes an older reader wrong. They are listed in the table in the order they were appended, and a
later field goes after them.

- Absent activity (a format 1 server): **Idle**. Idle claims least: it says nothing is running and nothing is wrong,
  and a send is always safe, since the server answers a send it cannot carry out with a notice in the session.
- Unknown entry kind: a **Notice**, a plain dim line with the entry's text. The text is never lost, and a notice
  claims nothing about who wrote it.
- Unknown activity: **Busy** (the pulsing light blue). Something a later server knows is going on; busy says work
  may be under way without claiming the friend is waiting (idle) or broken (down). A stop is always allowed.
- A format 1 link carries a Tool entry as a Friend entry (the line's text unchanged) and no activity.
- Absent instance (a server from before instances, or a format 1 link): **none**, empty. The window groups the session
  by its friend alone; the server, which holds the friend's instances, settles an empty instance to the friend's
  default instance (its first) and says so in a notice, so an empty one never decides where a driver runs.

### Agreeing on a format when the window connects

1. The window runs `env NEXUS_STATE_FORMAT=<its format> nexus-server new ...` over ssh. A server before formats ignores
   the variable.
2. `new` asks the server `new[ <client> <next>] format <n>`. A server before formats answers `ERROR unknown request;
   ...`, and `new` asks again without the format.
3. A server that knows formats answers `NEXUS CONNECT <port> <key> format <m>`, where `m` is the older of the two
   formats. It adds `format <m>` only when the window stated one. With no `format` in the answer, the link is
   format 1.
4. Both ends then read and write the state in `m`.
5. A floor refuses an incompatible end, in one plain line in the window: the server's `kOldestWindowFormat`, and the
   window's `kOldestServerFormat`. Both are 1 today, so every pairing works.

The actions (window to server) are unchanged and unframed (format 1). Their first change must come with a framed
format agreed the same way, under the same link format number.

### State files

`nexus --state FILE` and the tests' fixtures keep a state as the line `nexus-state <format>`, then the state's
`init_diff` in that format (`sync::stateFile`, `sync::readStateFile`). A file of a later format is read as a later
server's state is.

## The store (`src/store/store.h`)

| Kind | Format | Reader byte | What it is |
| --- | --- | --- | --- |
| record | 1 | | a friend tab's, by name: migrated at open |
| record | 2 | | by session, unframed, entry kinds 0 to 2 |
| record | 3 | 3 | by session; each change framed by its length, each entry framed in it; trailing fields skipped. Appended since: an info's frame ends with the instance (`str`), a driver's with the summary flag (`u8`) and its session's directory (`str`) |
| snapshot | 1, 2 | | friend tabs (1 before drivers): migrated at open |
| snapshot | 3 | | by session, unframed, entry kinds 0 to 2 |
| snapshot | 4 | 4 | by session; each session framed, each entry framed in it; trailing fields skipped. Appended since, after the driver: the summary flag, its session's directory, then the instance |

- Absent instance: **the friend's default instance**, settled at the server's start with a notice, as above.
- Absent summary flag: **none owed**. Absent session directory: **not known**, and the next driver resumes the
  harness session as before the field existed. Resuming claims least: it is what a server from before did, and a
  harness that has no such session in the directory starts a new one.
- An older server that rewrites the store (a compaction) drops the appended fields; a newer one then reads their
  defaults.

- A newer server reads every older format. Format 2 records and format 3 snapshots are read as they are, never
  rewritten. A friend-tab store is compacted into the current format at open.
- An unframed format holding an entry kind it never had is refused, naming the store, the record (or snapshot) and
  the kind. That is damage, not a later version.
- A framed record or snapshot of a later format is read. A change of a kind this build does not know is skipped
  whole; fields after the known ones are skipped; an entry kind it does not know is kept as it is, and the server
  shows it as a notice (`server/durable.cpp`).
- It is refused only when its reader byte names a format later than this build reads. The message names the store,
  the record and both formats, and says to run the newer nexus-server.

## The fixtures

Today's main formats are fixed as bytes that main's own code wrote, not as bytes this build claims main would write:

- `tests/sync/fixtures/main-state-1.init` and `.grow`, from `tests/sync/fixtures/main_state_fixture.cpp`;
- `tests/store/fixtures/main_store.h`, from `tests/store/fixtures/main_store_fixture.cpp`.

Each generator says how to build it against a worktree of main at 50565c6. `sync_unit` (`state_format_1_from_main`,
`state_format_2_reads_later_formats`, `state_files`, `link_formats_agree`), `store_unit_formats`,
`durable_unit_kinds` and `link_e2e_older_server` check the rules above; `sync_unit` `sessions_instance_appended` and
`store_unit_instance` check the appended fields both ways.
