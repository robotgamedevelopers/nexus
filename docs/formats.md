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

## The sessions state (server to window)

| Format | What it is |
| --- | --- |
| 1 | The first (main before formats): nothing framed, no activity, entry kinds Person 0, Friend 1, Notice 2. A reader of 1 reads exactly 1. |
| 2 | Each session and each entry framed by its length (`Writer::str`); the session's activity after its effort; entry kind Tool 3. Anything after the sessions, and after the known fields of a frame, is skipped. Appended since, at the end of the session's frame: its instance (`str`). |
| 3 | Format 2, then the open secret requests after the sessions (`docs/secrets.md`): a count, then each request framed by its length (its id, account, what is asked, the asker's session, the asker's friend, the asker's reason). The list is always written (count 0 for none), so a diff cut short before it is refused. Anything after a request's known fields, and after the list, is skipped. The actions up are framed from 3, with the secret dialog's answer (15) and cancel (16). A window of 2 receives no requests, and the server counts it as unable to take one; a secret action, which 2 cannot carry, is written as a stop naming session 0, refused by every server. |
| 4 | schema's variable tables (`schema/Nexus.schema`): the state is a `SessionsDiff` and the actions an `ActionsDiff`, written and read by the generated code. Every field, enum value and union arm rides under the hash of its name, so rules 1 to 3 hold by construction: a field a reader does not know is skipped, an enum value it does not know reads None, and both are counted (`TableReport.unknown`), which the window and the server log when the count changes. None reads as the plainest thing known: an entry kind as a Notice, an activity as Busy, an action kind as unknown (which the server refuses alone, never the link). It says the same as format 3. |

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
- Absent secret requests (a link of format 2 or older): **none**. A window with none shows no dialog and sends no secret
  action; the server, which knows the link's format, never offers that window a request.
- Absent instance (a server from before instances, or a format 1 link): **none**, empty. The window groups the session
  by its friend alone; the server, which holds the friend's instances, settles an empty instance to the friend's
  default instance (its first) and says so in a notice, so an empty one never decides where a driver runs.

Text in format 4 is schema's text: well-formed UTF-8 with no NUL, or the whole diff would not load. The server writes
what it holds with each malformed byte, and each NUL, shown as U+FFFD (the replacement character), so a friend's
stray byte costs one character on screen, never the link. An entry whose text is not clean is sent whole each time,
never as growth, so the window's copy is always the server's text as shown. A secret's value rides as bytes, never
as text, and is never changed.

### Agreeing on a format when the window connects: the Hello

A Hello (`schema/Nexus.schema`) is two numbers: `proto`, the protocol an end speaks (its state format, 4 today), and
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
`store_unit_instance` check the appended fields both ways. `wire_unit_nexus_sessions`, `wire_unit_nexus_actions` and
`wire_unit_nexus_hello` check format 4 and the Hello both ways against a later version; `sync_unit` `hello_min_peer`,
`sessions_unknown_kind_is_a_notice` and `sessions_text_not_utf8` check the refusal, the unknown kind and the text
rule.

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

Encode and decode on the link thread, never the window's frame thread (`tests/sync/wire_bench`, release build, mean of
one): a typical frame's diff (one open entry grown by 120 bytes, 404 bytes) 0.023 ms to encode and 0.002 ms to
decode; a first state of sixteen sessions of sixteen 1 KiB entries (267 KB) 0.18 ms and 0.26 ms; eight 200-byte sends
up 0.023 ms and 0.003 ms. Format 3 is faster per message (0.001 ms for the typical diff), and every one is far inside
a 120 Hz frame's 8.3 ms.
