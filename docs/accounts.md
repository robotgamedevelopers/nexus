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
- When a session's account reaches its usage limit, the shipped server policy picks the friend's next signed-in linked account,
  in link order, and tells the session. If there is none, the session is told that too, and stays.
- When a change takes a friend out of friends.json, the server stops the work running in each of its sessions (the
  turn open gets the person's stop; its driver is ended) and tells each session once. The sessions are kept as they
  are (account, harness, instance, transcript and queued prompts); while the friend is out nothing starts in them and
  a prompt sent is refused, and they read as limited. When the friend is added again, its sessions are settled like any
  other, take the limit its entry says, and what is queued goes on.
- When a change leaves a session on an account that is no longer linked to its friend (an unlink, an account
  removed), the server moves it at once to the friend's default if that is signed in, else to its first
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
in-app sign-in, never through an AI and never by editing a file. The logins stay under your home whatever `--dir` says:
the server's sign-in check and the drivers read the one path (`src/server/home.h`, `accountsDir`).

The logins' directory holds secrets only, and the AI a driver runs is kept from reading it, or the token, best effort:
its shell, its file tools and a fetch are walled off from it, and none of the routes the tests try reads it
([secrets.md](secrets.md), "The token wall", says which layer stops which route, and what stays open). The Claude
driver keeps Claude's own state (its settings, sessions, memory) apart, in `~/.nexus/server/harness/claude/<name>/`,
which the AI can read and write. A driver from before this kept that state in the login's directory; the first start of
this one copies only the person's `settings.json`, `settings.local.json` and `CLAUDE.md` from there, and nothing else:
not the transcripts and memory (`projects/`), the shell snapshots, the history or `.claude.json`, which from before the
wall may hold the token, and never a file the secret dialog stored. Each of the three is copied only when it is a
regular file of one name (a hard link is refused), at most 1 MiB, and does not hold the account's token; otherwise the
driver refuses to start in one line naming the file and the fix, and makes no directory. Those not copied stay where
they were, under the wall, and a session from before does not resume. Editing `accounts.json` and `friends.json` stays open to
the AI: they hold no secret.

## accounts.json

```json
{
  "version": 2,
  "accounts": [
    {
      "name": "work",
      "provider": "claude",
      "billing": "subscription"
    },
    {
      "name": "home",
      "provider": "claude",
      "billing": "subscription"
    },
    {
      "name": "codex-main",
      "provider": "codex",
      "billing": "api"
    }
  ]
}
```

| Field | Where | Required | Allowed values |
|---|---|---|---|
| `version` | top | yes | the whole number `2` (see Versions) |
| `accounts` | top | yes | a list of accounts, in any order you like; `[]` for none |
| `name` | each account | yes | 1 to 64 letters, digits, `-`, `_` or `.`, not starting with `.` or `-`; unique among the accounts |
| `provider` | each account | yes | 1 to 32 lowercase letters, digits or `-`, as `claude` or `codex` |
| `billing` | each account | no (`subscription`) | `"subscription"` (the provider's subscription plan) or `"api"` (billed at API rate, per token); a friend on an `api` account is always limited (friends.json's `limit`) |

The name is also the directory of its login, `~/.nexus/server/accounts/<name>/`.

## friends.json

```json
{
  "version": 2,
  "limit_keeps_out": [
    "/data"
  ],
  "friends": [
    {
      "name": "claude",
      "limit": false,
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
      "limit": true,
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
| `version` | top | yes | the whole number `2` (see Versions) |
| `limit_keeps_out` | top | no (none) | a list of absolute directories, each once, never `"/"`: more places every limited friend's AI is kept out of, on top of the ones it always is (below, "limit"), as a second data volume or a network mount; it can only add, never let a limited friend read more. Written back only when it holds one |
| `friends` | top | yes | a list of friends, at most 256, in every version; `[]` for none. A file with more is refused whole, with the count and the limit, and nothing changes (a version 1 file too, before any upgrade); adding one in the window at the limit is refused the same way |
| `name` | each friend | yes | 1 to 64 letters, digits, `-`, `_` or `.`, not starting with `.` or `-`; unique among the friends |
| `limit` | each friend | no (`true`) | `true` or `false`, without quotes: whether the friend's AI reads and writes only in its instance's directory (below, "limit"); `false` is refused for a friend that links an account with `"billing": "api"` |
| `accounts` | each friend | no (none) | a list of account names from accounts.json, each once; the shipped account rotation policy uses this order (see [customizing policy](policy.md)) |
| `default` | each friend | no (none) | one of the friend's `accounts`, or `null` for none |
| `instances` | each friend | no (none) | a list of instances, in order; the first is the friend's default instance |
| `name` | each instance | yes | 1 to 64 letters, digits, `-`, `_` or `.`, not starting with `.` or `-`; unique among its friend's instances (two friends may use the same name) |
| `machine` | each instance | yes | the machine it runs on, by its name in the fleet, as `studio` (1 to 253 letters, digits, `-` or `.`, not starting with `-` or `.`; never a user, a port or an address with them); this server's own when it equals, ignoring case, the server's machine (below) |
| `dir` | each instance | yes | the absolute path, on that machine, of the directory it works in (starting with `/`, printable, at most 4096 bytes) |

A friend is known by its `name`, everywhere: the window's tab, the saved tab order, a kept draft. Renaming a friend in
the file is a remove of the old name and an add of the new: the old name's tab goes (its draft kept under the old name)
and the new name gets a new tab, and the old name's sessions stay with the old name, which is out of the file. Rename as
one change, which would carry the sessions, the tab and its draft to the new name, is not built (`docs/roadmap.sexp`
`friend-rename`).

The two files must agree: every account a friend links to is set up in accounts.json, and every default is one of its
friend's accounts. Moving an instance (its `machine` or `dir`) is an edit like any other, and every session of it is
told. A session's driver that is running ends once no turn is open (an open turn runs to its end first), and the next
starts where the instance now is: in the same directory it resumes the conversation; in another it starts afresh, and
its first prompt carries a summary of the conversation so far (the harness keeps its conversations by directory). An
instance moved to another machine starts no driver here until a driver on another machine exists (roadmap work). A
session that names no instance while its friend has some belongs to the friend's default instance, and is told. A
session whose friend has no instances (every one removed, say) works here, in the directory of its friend's line in the
drivers file. A file that is empty or missing means none.

The limit of 256 friends is worked out from the largest change the server sends a window (4 MiB): 256 names of at most
64 bytes are about 20 KiB with their framing, well inside it. It is not a measured number.

Taking a friend out of the file (and running `nexus-server config reload`) stops its work in every one of its sessions,
as a stop in the window would: a turn running is stopped, and then its driver is ended. Each such session gets one
notice saying so. The sessions themselves, with their transcripts, account, instance and queued prompts, are kept on
the server as they were. While the friend is out, no driver starts for them, nothing is taken from their queues, and a
prompt sent to one is refused with a notice. Put the friend back in the file and reload, and its sessions are settled
like any others (an account no longer linked moves, and so on), and their queued prompts go on.

The server's machine is the name the `machine` fields are compared with. It is the server's short host name (the
`hostname` up to its first `.`), unless the server's config, `~/.nexus/server/config` (one `key value` per line,
`#` for a comment), says `machine <name>`, the name the fleet knows it by. An instance is on this server's machine when
its `machine` equals, ignoring case, either that name or the full host name: on a Mac whose `hostname` is
`studio.local`, both `studio` and `studio.local` are this machine. When the host name cannot be read and the config
names none, the server has no machine: no instance is on it, and it says so in its log.

The same config may say `claude <absolute path>`: the `claude` program a claude account's sign-in runs
(`claude setup-token`, docs/secrets.md "Sign-in"). Without it the sign-in runs the first `claude` on the server's PATH.

To have an account signed in, an AI runs `nexus-server config sign-in <account>` (over ssh too). It waits, asking the
server once a second, and prints one line: `signed in` (at once when the account already is), or `refused: <why>`
(no such account, a provider without in-app sign-in, no server, or not signed in within the sign-in's 10 minutes and
one more). The person signs the account in in the browser from nexus's window; the AI sees nothing of the sign-in. A
driver started after a sign-in runs under its friend's `limit` as every driver start does (below).

## limit: a friend kept to its directory

`"limit": true` keeps the friend's AI, its shell and its file tools, inside the directory of the instance its session
runs in: it reads and writes there, and nowhere else people keep files. `"limit": false` is the AI as it ran before
the field existed: under the token wall (docs/secrets.md), but free to read the rest of the home directory.

- **A friend added is limited.** One added in the window, or written into friends.json without a `limit`, is `true`,
  whatever the file's version: a friend added by hand to a version 1 file, not yet reloaded, reads limited too (only
  the friends a version 1 file already held when it was committed read unlimited, as they ran). Set `"limit": false` on
  a friend you want unlimited (yourself as administrator, say) by editing the file.
- **A fresh install's first friend is unlimited.** The friends a new server makes from its drivers file, at its first
  start with no accounts set up (or the upgrade of an install from before accounts), are `"limit": false`, as they ran:
  that is the person setting nexus up, as administrator. Every friend added after that is limited.
- **A friend on an API-billed account is always limited.** A friend that links any account with `"billing": "api"`
  cannot be `false`: `config check` and a reload refuse it, naming both files and the fix; linking such an account to an
  unlimited friend in the window is refused the same way. (The fleet's own workers that run models at API rate outside
  nexus run under their own sandbox tool, not this field.)
- **What it keeps out**, on macOS and Linux (WSL2 too): the home directory and the places people keep files (macOS
  `/Users`, `/Volumes`; Linux `/home`, `/root`, `/mnt` (WSL2's Windows drives), `/media`, `/run/media`, `/srv`), so
  other projects, documents, `~/.ssh`'s keys, other friends' directories and other accounts' state; and every write
  outside its directory and the temporary directories. What stays readable, and the costs, are in docs/secrets.md
  ("A limited friend").
- **More places to keep out.** Where people keep files somewhere else too (a second volume such as `/data`, a network
  mount, a WSL2 automount root moved off `/mnt`), name each directory in friends.json's top-level `"limit_keeps_out"`:
  every limited friend is kept out of it as well, by the shell and the file tools alike. A directory that does not
  exist on this machine is left out. It only adds: nothing in it lets a limited friend read more, and it changes
  nothing for an unlimited friend. A reload that changes it tells each limited session; a directory added stops a
  turn already running, as a tightening does (below), and one removed waits for the turn's end. nexus reads none of
  this from the environment: the server passes each to the driver as `--keep-out <dir>` (docs/driver-protocol.md).
- **Where it cannot start.** A limited session does not start, with one line saying why where a session's start
  refusals already show, when its directory does not exist, is the home directory or the root, holds `~/.nexus`, or is
  inside `~/.nexus` or `~/.ssh`; on a platform other than macOS and Linux; or when the drivers file names another Claude
  (`--claude`). It never starts unlimited instead.
- **Tightening takes effect at once; loosening at the turn's end.** When a reload makes a session limited (its
  friend's `limit` set to `true`, or its account's `billing` set to `"api"`), a turn running on a driver started
  without the limit is stopped at once, as the stop button stops it (the driver has the stop's deadline to finish, then
  it is killed), the transcript says why, and the next turn starts limited: an old, weaker limit never lasts past that
  stop. When a reload lifts the limit, the open turn finishes as it started, and the next starts unlimited. A change of
  a friend's `limit` is also told in each of its sessions' transcripts; a change of an account's `billing` is told only
  where it stops a turn (the server's reply and log list it either way).
- **A driver must say it supports the limit.** A limited session runs only on a driver whose hello lists the `limit`
  feature (docs/driver-protocol.md); any other driver is ended before it is given a prompt, and the session says why.

The server decides a session's limit each time it starts the session's driver, from the files as they are then: limited
when the friend is limited, when its account is billed at API rate, when the friend is not in friends.json at all, or
when its account is not in accounts.json; and limited when it cannot tell. models/Accounts.tla checks the rules (no
driver on an API-billed account starts unlimited; none starts weaker than its friend's limit); models/Supervisor.tla
checks that no session keeps running with a weaker limit than its friend's (tightening stops the open turn, and a
driver started under another limit is ended).

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
onto its friend's default account and instance, told, so that it runs as it ran before. The upgrade's commit leaves a
mark in the repository, taken off only once every session has moved: a server that stops before then (a store that
failed, say) moves them at its next start, and a session already moved is not moved twice.

### A remote (optional)

To keep the history somewhere else too, make a **private** repository of your own (the suggested name is
`<owner>/nexus-config`) and give it to the server:

- `nexus-server config remote <url>` sets it (for example `git@github.com:<owner>/nexus-config.git`). A url with a
  credential in it (a password, as `https://user:secret@...`, or any user part on https, as `https://token@...`) is
  refused: credentials never go in the remote URL; nexus pushes with the machine's git credentials (below). An ssh
  login name, as the `git@` above, is not a credential. A remote that holds one anyway (set with git by hand) is
  printed and logged with its user part left out.
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

## How long a login lasts

The owner's rule (2026-11-14): a person pastes a key or signs in once, ever; nexus never loses it and never asks again.
A login that can be renewed without a person, nexus renews before it ends, with no prompt. One that cannot, nexus knows
the end of, and says so well ahead. The only time it asks for a second paste is a login the provider revoked or that
ended with no way to renew it, and its dialog says which. Each account's login is that account's own: it is stored in
that account's directory on the server it was pasted into, and used only for that account's sessions.

| provider | what is stored | how long it lasts | renewed by | what nexus does | sources |
|---|---|---|---|---|---|
| `claude` | `oauth-token`: the token `claude setup-token` prints, for the account's Claude subscription | one year from when it was made | nothing: it has no refresh token; a person runs `claude setup-token` again | built: counts the year from the file's time, tells the account's sessions 30, 7 and 1 days ahead, and on the day asks for a new one in the window, saying why | code.claude.com/docs/en/authentication ("Generate a long-lived token": one year, no refresh); code.claude.com/docs/en/errors ("OAuth token has expired", "OAuth token revoked") |
| codex (ChatGPT sign-in) | `~/.codex/auth.json`: access, ID and refresh tokens | access tokens are short; the CLI refreshes about every 8 days and on a 401, and the refresh token rotates | the Codex CLI itself; `codex login --device-auth` signs in without a browser on the machine | roadmap (`login-life-codex`): no codex driver yet | the Codex authentication docs; openai/codex `codex-rs/core/src/auth.rs` |
| gemini (Google sign-in) | `oauth_creds.json`: access and refresh tokens | access tokens about an hour | google-auth-library refreshes them from the refresh token, unattended | roadmap (`login-life-gemini`): no gemini driver yet | geminicli.com/docs/get-started/authentication; google-gemini/gemini-cli `packages/core/src/code_assist/oauth2.ts` |
| an API key (Anthropic Console, OpenAI, Gemini, xAI) | the key | until it is revoked; an xAI key may carry an optional expiry chosen when it is made | nothing needed | roadmap (`login-life-api-keys`): no provider of nexus stores a key yet | each provider's key docs (docs.x.ai for the optional expiry) |

**Claude, in full.** A claude account's login is the long-lived token from `claude setup-token`, which runs Claude Code
on the account's Claude subscription rather than API billing. Anthropic documents it as lasting one year, with no
refresh: the only renewal is to run `claude setup-token` again. (Claude Code's own interactive `/login` stores an access
token, a refresh token and an expiry, and refreshes them itself; how to refresh is not documented for other programs,
so nexus does not store or refresh those.) So nexus-server:

- Counts the year from when the file was written (its modification time). A token made some days before it was pasted
  ends that much sooner than the server counts; the 30 days' notice covers that.
- Checks every stored claude login when it starts and once an hour. 30 days, 7 days and 1 day before the end it tells
  every session on the account, and `server.log`, the date and that nothing changes until then; each notice once (a
  restart tells the one due then again).
- On the day, signs the account out and asks for a new token in the window: "a1's Claude login ended on 2027-11-14, a
  year after it was pasted, and cannot be renewed: run claude setup-token and paste the token it prints".
- When the driver reports the token was refused before its year is out, asks for a new one, saying it was refused by
  the provider (revoked, or no longer valid), with the provider's words in `server.log`.
- When the file is there but cannot be used as it lies (a symbolic link, another user's, open to other users, not
  readable), asks for nothing: a paste would not fix it. It tells the account's sessions the fix (`chmod 600`, remove
  the link) instead.
- When the file is not one token (two words, empty), asks for a new one, saying so.

At the start, an account whose login is missing, not one token, unusable as it lies, or past its year is not signed in.

## Known limits

- The upgrade's mark (above, under History) is taken off once every session has moved. If the server cannot remove it
  (the repository's `.git` not writable, say), it logs why, and each later start applies the friends' defaults again to
  the sessions then on no account. Sessions on an account are never moved by it. Fix what the log names, and the next
  start removes the mark.

## Versions

Both files carry `"version"`, from the first release. A nexus-server reads its own version and every older one; an
older file is upgraded in place when the server starts (your entries kept as they were, written in the new version, and
committed as an upgrade, which the server's log reports), unless the files hold edits not yet applied: those are never
written over, and the next reload writes the new version. `config check` never writes anything, an upgrade included.
Version 2 added `billing` (accounts.json) and `limit` (friends.json). A version 1 file is read with every account billed
by subscription and every friend it held when committed **not** limited, as it ran before (a friend added by hand to a
version 1 file while the edit is pending reads limited: it never ran), and the upgrade writes each friend's `"limit"`
and each account's `"billing": "subscription"` into the files, so that the choice is plain to see and to change.

**A downgrade drops the limit for everyone.** An older nexus-server refuses the version 2 files, so it holds no
accounts and no friends, and its Claude driver has no limit: every session it runs is unlimited. Do not run an older
nexus-server against version 2 files while any friend is limited; if you must go back, stop the friends that are
limited first (or remove their instances), and expect every session to run as it did before the limit existed.

A file whose version is newer than the server reads is refused
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
