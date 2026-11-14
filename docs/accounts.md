# Accounts and friends: the files, and how to change them

This page is for a person, or the AI they talk to, who sets up nexus-server's accounts and friends. It is all you need:
every field, what it may hold, an example of each file, the commands, and what the server says back. nexus has no
settings screen. You change these files by talking with your AI, and the AI edits them, checks them and applies them
with the commands below.

## What they are

- An **account** is one login with one **provider**: the harness its sessions run (`claude` today; others as their
  drivers come). It is set up on its own, by name.
- A **friend** is who you talk to in a tab. A friend links to any number of accounts, of any providers, in an order,
  and one of them may be its **default**. An account may serve any number of friends.
- An **instance** of a friend is that friend on one machine, working from one directory: `rowan` on `studio` in
  `/Volumes/nova/ai/rowan`, `rowan` on `studio` in a rocketnet checkout, `rowan` on the `macbook`. A friend has any
  number of instances; the first is its default. The friend (its name, its accounts and its default account) is
  shared by all its instances. A machine is the one the instance runs on now: moving it is an ordinary edit.
- A **session** belongs to one instance of its friend, or to none when the friend has none. A new session takes the
  instance of the session it copies when that is the same friend's and still one of its instances, else the
  friend's default instance. Its driver works in the instance's directory. Today a driver runs only on the server's
  own machine: a prompt in a session whose instance is on another machine is not sent, and the session is told so
  (a driver on the instance's machine is roadmap work). When an edit removes a session's instance (or its friend),
  the session moves to the friend's default instance, or none, and is told. Every session's friend and instance
  reach the windows, so a window can show a tab per friend or per instance, as its own config says.
- A **session** (a conversation with a friend) runs on one of its friend's linked accounts, or on none (the harness's
  own login). A new session takes the account of the session it copies when that account is linked to its friend,
  else the friend's default. A session's harness is its account's provider, so switching a session's account can
  switch harness.
- When a session's account reaches its usage limit, the server moves it to the friend's next signed-in linked account,
  in link order, and tells the session. If there is none, the session is told that too, and stays.
- When a change leaves a session on an account that is no longer linked to its friend (an unlink, an account removed,
  a friend removed), the server moves it at once to the friend's default if that is signed in, else to its first
  signed-in linked account, else to none, and tells the session. When an account's provider changes, its sessions take
  the new harness and are told. A default that is unlinked or removed becomes none.
- Which accounts are signed in is the server's own state; it is not in these files. When the server starts, and when an
  account is set up, it is signed in if its login is there (for a `claude` account, the token file
  `~/.nexus/server/accounts/<name>/oauth-token`). From then on the drivers report it: a driver that starts on the
  account and reports its credential signs it in; one that reports no credential, or a failed authentication, marks it
  as needing sign-in; one that reports a usage limit (`usage-limit`, `docs/driver-protocol.md`) moves its session on,
  as above.
- A session on no account whose friend's driver needs one (the Claude driver's line passes `--account`) starts no
  driver: its prompt is not sent, and it is told to link an account to the friend and set the session to it.

These rules are `models/Accounts.tla`, and the server's code is checked against it.

## Where they are

```
~/.nexus/server/nexus-config/accounts.json
~/.nexus/server/nexus-config/friends.json
```

`nexus-server config path` prints the directory and both files. (With `--dir DIR`, they are under `DIR/nexus-config/`.)

**No secret ever goes in these files.** They hold names only. Each account's login lives in
`~/.nexus/server/accounts/<name>/` (a 0700 directory of 0600 files), outside this directory, and is set up by the
in-app sign-in, never through an AI and never by editing a file.

## accounts.json

```json
{
  "version": 1,
  "accounts": [
    {
      "name": "work",
      "provider": "claude"
    },
    {
      "name": "home",
      "provider": "claude"
    },
    {
      "name": "codex-main",
      "provider": "codex"
    }
  ]
}
```

| Field | Where | Required | Allowed values |
|---|---|---|---|
| `version` | top | yes | the whole number `1` (see Versions) |
| `accounts` | top | yes | a list of accounts, in any order you like; `[]` for none |
| `name` | each account | yes | 1 to 64 letters, digits, `-`, `_` or `.`, not starting with `.` or `-`; unique among the accounts |
| `provider` | each account | yes | 1 to 32 lowercase letters, digits or `-`, as `claude` or `codex` |

The name is also the directory of its login, `~/.nexus/server/accounts/<name>/`.

## friends.json

```json
{
  "version": 1,
  "friends": [
    {
      "name": "claude",
      "accounts": [
        "work",
        "home",
        "codex-main"
      ],
      "default": "work",
      "instances": [
        {
          "name": "studio",
          "machine": "studio",
          "dir": "/Volumes/nova/ai/rowan"
        },
        {
          "name": "rocketnet",
          "machine": "studio",
          "dir": "/Users/me/rocketnet"
        },
        {
          "name": "macbook",
          "machine": "macbook",
          "dir": "/Users/me/work"
        }
      ]
    },
    {
      "name": "reviewer",
      "accounts": [
        "home"
      ],
      "default": null,
      "instances": []
    }
  ]
}
```

| Field | Where | Required | Allowed values |
|---|---|---|---|
| `version` | top | yes | the whole number `1` (see Versions) |
| `friends` | top | yes | a list of friends; `[]` for none |
| `name` | each friend | yes | 1 to 64 letters, digits, `-`, `_` or `.`, not starting with `.` or `-`; unique among the friends |
| `accounts` | each friend | no (none) | a list of account names from accounts.json, each once; the order is the order a rotation at a usage limit goes through them |
| `default` | each friend | no (none) | one of the friend's `accounts`, or `null` for none |
| `instances` | each friend | no (none) | a list of instances, in order; the first is the friend's default instance |
| `name` | each instance | yes | 1 to 64 letters, digits, `-`, `_` or `.`, not starting with `.` or `-`; unique among its friend's instances (two friends may use the same name) |
| `machine` | each instance | yes | the host name of the machine it runs on, as `studio` (1 to 253 letters, digits, `-` or `.`, not starting with `-` or `.`; never a user, a port or an address with them); this server's own host when it is the short form of `hostname` |
| `dir` | each instance | yes | the absolute path, on that machine, of the directory it works in (starting with `/`, printable, at most 4096 bytes) |

The two files must agree: every account a friend links to is set up in accounts.json, and every default is one of its
friend's accounts. Moving an instance (its `machine` or `dir`) is an edit like any other: the next session of it starts
there. (What a running session's driver does on a move, restart there resuming when the directory is the same
repository and start fresh with a summary when it is not, is roadmap work; today the move takes effect when its driver
next starts.) A file that is empty or missing means none.

The server writes both files in one layout: two-space indent, one field per line, keys in the order above, the lists in
the order they hold, and a newline at the end. Write them the same way and a save never reshuffles your edit (any valid
JSON is read; this is only how it is written back).

## Changing them

1. Edit the files (or ask your AI to).
2. `nexus-server config check` reads them and changes nothing. It prints every problem as
   `error: <file>:<line>: <what is wrong>; <what to change>`, or the changes a reload would make
   (`would apply: ...`) and `OK`.
3. `nexus-server config reload` applies them. The server decides, against what it holds when it gets the request; it
   prints one line for each change it made (`applied: ...`, sessions moved included) and `OK`, or each reason it
   refused (`refused: ...`) and `ERROR nothing was applied; the server keeps the accounts and friends it had`. The same
   lines go to the server's log, `~/.nexus/server/server.log`. An edit is applied whole or not at all.

A reload is explicit: the server never applies a file because it changed on disk, so a half-finished edit is never
applied. While the files hold edits not yet applied, nothing else writes them: an account change from a window and a
restore are refused, and an upgrade waits, each saying so; reload the edit, or drop it with
`git -C ~/.nexus/server/nexus-config checkout -- .`.

An apply is all or nothing on disk as well: if writing a file, staging or committing fails, the files and git's index
are put back as they were and nothing is committed. Once it is committed it is applied; if the server's own store then
fails while sessions move, the reply says so, the server stops, and at its next start it moves them against what was
committed.

Examples of what the server says:

```
$ nexus-server config check
error: /home/me/.nexus/server/nexus-config/friends.json:4: this is not JSON; fix the text at this line; docs/accounts.md has an example of the whole file
ERROR 1 problem; nothing was changed

$ nexus-server config check
error: friends.json: the friend "claude" links to the account "gone", which accounts.json does not set up; add the account there, or take it out of "claude"'s "accounts"
ERROR 1 problem; nothing was changed

$ nexus-server config reload
applied: friend claude's accounts [work, home] -> [home]
applied: friend claude's default work -> none
applied: session 3 (claude): account work -> home, harness claude
OK
```

### From another machine

The AI you talk to may run on another machine than nexus-server. Run the same commands over ssh to the server's host;
they need nothing else:

```
ssh server-host nexus-server config path
ssh server-host cat ~/.nexus/server/nexus-config/friends.json
ssh server-host nexus-server config check
ssh server-host nexus-server config reload
```

Edit the files there (over ssh, or copy them back and forth), then check and reload. Every decision is the server's,
made when the reload reaches it.

## History, and undoing a change

`~/.nexus/server/nexus-config/` is a git repository of its own, made by nexus-server on its first start. Every change the
server applies is one commit whose message says what changed and who asked: an account action from a window, a hand
edit applied by a reload (the person's or their AI's), a version restored, or an upgrade of the files' version.

- `nexus-server config history` lists the commits, newest first: `<commit> <date> <what changed and who asked>`.
- `nexus-server config restore <commit>` applies the files as that commit held them, as a new commit. History is never
  rewritten, so a restore can itself be undone the same way. It is refused while the files hold edits not yet applied.

Its `.gitignore` admits only `.gitignore`, `accounts.json` and `friends.json`, and every commit names only
`accounts.json` and `friends.json`. git runs as a child process with a clean environment: no system or global git
config (your `~/.gitconfig` is never read), no hooks, no signing, no prompts, never above this directory, the
transports file, ssh and https only, the identity `nexus`, and a deadline on every call.

A server from before accounts has no accounts.json: at its first start with this version, the accounts and friends are
taken from the drivers file (each driver line's `--account <name>` becomes an account whose provider is that line's
harness, linked to that harness's friends as their default; and each friend gets one instance, named after this
machine, on this machine, in the directory its `friend` line gives), committed as an upgrade, and each session moves
onto its friend's default account and instance, told, so that it runs as it ran before.

### A remote (optional)

To keep the history somewhere else too, make a **private** repository of your own (the suggested name is
`<owner>/nexus-config`) and give it to the server:

- `nexus-server config remote <url>` sets it (for example `git@github.com:<owner>/nexus-config.git`).
- `nexus-server config remote` prints it, or `none`.
- `nexus-server config remote --clear` removes it.

With a remote, every commit is followed by a push. The push runs in the background, as a child process the server
watches with a 60 s deadline, so nothing waits on it: the reload replies at once (`applied: committed; pushing to <url>
in the background ...`), and the push's result goes to the server's log (`nexus-server: config push: pushed to <url>`,
or why it failed). A push that fails or runs past its deadline never undoes the change; the next commit pushes again.

A push uses exactly one of two credentials, and never asks for anything:

- an ssh remote (`git@github.com:...`): the ssh keys or agent of the user nexus-server runs as, with
  `BatchMode=yes` and a 15 s `ConnectTimeout`;
- an https remote (`https://github.com/...`): the GitHub CLI's stored login, through `gh auth git-credential`
  (`gh auth login` once, as that user).

Before setting a remote and before every push, the server checks every file in the repository's whole history, not
only the files tracked now. If any commit ever held a file other than its three, nothing is pushed, ever: a push would
publish that commit. The message names the file and the two ways out: make a new repository (move the directory
aside, start nexus-server, copy the two files in, `nexus-server config reload`), or rewrite the history so that no
commit holds it; and if it was a credential, replace the credential. The server never creates the remote repository.

## Versions

Both files carry `"version"`, from the first release. A nexus-server reads its own version and every older one; an
older file is upgraded in place when the server starts (your entries kept as they were, written in the new version, and
committed as an upgrade, which the server's log reports), unless the files hold edits not yet applied: those are never
written over, and the next reload writes the new version. `config check` never writes anything, an upgrade included.
(Version 1 is the first, so no file is older yet.) A file whose version is newer than the server reads is refused
with a message saying so, and left exactly as it is: run the newer nexus-server, or update this one. The version only
rises when a change cannot be read by an older reader.

## For a driver's AI

A driver tells the AI it runs where these files are and how to change them (`configBrief` in
`src/server/config_repo.h` holds that text, for the drivers to pass on: part 4 of the sign-in work in
`docs/roadmap.sexp`).

## Client config, later

The window's own config, `~/.nexus/client`, will be configured the same way: documented, human-editable JSON with a
`version`, a check that changes nothing, an apply that reports what it applied or refused, and history. It stays on
each machine: the server never stores, caches or syncs a client's config. When your AI (talking to the server) changes
a client's config, the server only relays the change to the client that asked, and that client applies it
(`docs/roadmap.sexp`, the client-config rows).
