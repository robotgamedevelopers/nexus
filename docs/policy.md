# Customizing Nexus in your fork

The window and server each run Starlark locally. Edit the files under `policy/rules/` in your fork, preview
an event, apply the revision on the machine running that side, and commit the files. The server uses the new revision on its next event. The window picks it up at a frame boundary after its background reader compiles it. Client decisions work when the server is offline.

| File | Used for |
|---|---|
| `client.star` | Local UI decisions |
| `server.star` | Decisions about server state |
| `config.json` | JSON object passed to both scripts |
| `client/`, `server/` and shared modules | Application code. Imports such as `load("client/window.star", "handle")` resolve from the rules directory. Apply includes the transitive imports for that side. |

Each entry point can export `handle(event, state, config)` for application events and `decide(event, context, config)` for scalar decisions. An entry point from an earlier fork that exports only one function inherits the other from the bundled defaults. The host supplies the event name and a JSON object
describing its current state. Starlark returns a JSON-compatible value. Each hook documents which values the
engine can apply. Modules can load other files from the same bundle. Scripts receive their inputs through the
function arguments and have no host filesystem, environment, network or process API. Keep credentials out of the fork.

## Edit, preview and apply

Build the window or server once with Cargo available. CMake also produces `build/nexus-policy`.
From the fork, preview the shipped choices with sample state:

```sh
echo '{"current":0,"delta":1,"min":-3,"max":4}' |
  build/nexus-policy preview client policy/rules text_zoom
echo '{"current":"one","linked":["one","two"],"eligible":["two"]}' |
  build/nexus-policy preview server policy/rules account_after_limit
```

The first command prints `1` and the second prints `"two"`. Change the source or config and repeat.
Then apply on the machine running the corresponding host:

```sh
build/nexus-policy apply client policy/rules
build/nexus-policy apply server policy/rules
git add policy/rules
git commit -m "Customize Nexus policy"
```

Each apply compiles that side's entry point and imports. It atomically replaces
`~/.nexus/client/policy.bundle.json` or `~/.nexus/server/policy.bundle.json` only on success. Pass a host
directory as the last argument for a server started with `--dir`. Windows uses the native user's client
directory and the WSL user's server directory. On separate machines, check out the same commit on each and
apply locally. To roll back, revert the policy commit and apply again. Without an applied bundle, each host
uses the rules packaged with its binary.

## Overlay opacity

Set `"overlay_opacity": 0.60` in the client’s `config.json` and apply the client profile. The shipped
`client/window.star` returns it as a number from 0 (clear) through 1 (opaque). A profile that omits the field
gets 0.60. The overlay and terminal use the same dark background. Text and the ordinary window stay opaque.

## Tab and account navigation

The client window event supplies `active_tab` (zero-based) and `tab_count` in current strip order. Stock
`client/window.star` binds Command-Left and Command-Right to the previous and next positions, wrapping at both
ends. Numbered shortcuts still select positions directly. In client `config.json`, `tab_navigation` maps chords
to signed offsets.

On a friend tab, Command-Up and Command-Down open the linked-account picker and highlight the previous or next
account. Up and Down continue through the list, wrapping at either end. Enter confirms the highlighted account.
Clicking a row confirms that account directly. Escape cancels and keeps the overlay and draft open. A change of
friend, session, current account or connection dismisses the picker. If the server reorders its accounts, the
highlight follows the same account ID. The server validates an exact account selection before applying it.

When the server offers automatic selection for a friend with multiple linked accounts, the first menu row is
**auto**. The tab caption also shows the concrete account currently in use. The sign-in card always identifies
that concrete account. Set `account_auto_label` in the client configuration or return it from the Starlark
`app.window` event to customize the mode’s display name.

The stock Starlark window program reads two optional maps from the fork’s client `config.json`:

```json
{
  "account_picker_shortcuts": {"Command+ArrowUp": "previous", "Command+ArrowDown": "next"},
  "account_picker_keys": {"ArrowUp": "previous", "ArrowDown": "next", "Enter": "confirm", "Escape": "cancel"}
}
```

A fork can emit `["Command+K", "account_picker", "open"]` to open on the current account. The window context
field `account_picker_open` lets the program scope other shortcuts to the open menu. Opening, navigating and
cancelling are local actions. After confirmation the server reports the selected account. For an account that
needs sign-in, a separate Enter or click on the sign-in card starts the provider flow.

An existing `account_shortcuts` map still sends its configured `account_command` actions immediately. For example,
`"account_shortcuts": {"Command+ArrowDown": "advance"}` asks the server’s
`decide("account_command", context, config)` to choose an account. Server config
`"account_navigation": {"steps": {"advance": 1}, "wrap": false}` stops at the final account. If a profile specifies
both maps for the same chord, `account_picker_shortcuts` takes precedence.

The applied stock profile sets `"account_default": "first"` for new conversations. Profiles without this option
retain their friend’s configured `preferred` default, including an empty one. New sessions copied from a linked
source retain the source’s explicit account.

## Events available in the native hosts

| Host and event | Context | Result |
|---|---|---|
| Client `text_zoom` | Integer `current` text scale step, signed key `delta`, inclusive `min` and `max` supported by the renderer | An integer in range. On an invalid answer the window keeps the current scale and reports the error. |
| Server `account_after_limit` | `current` account name, `linked` names in configuration order, and `eligible` names of other signed-in linked accounts | An eligible name, or `None` to stay. An invalid answer keeps the session on its current account and adds a notice. |

The shipped client advances a tab by the key's delta. A server-side relative account command uses the linked accounts in order.
The server reads a bundle at the decision and reuses the compiled module while its contents are the same. The window reads and compiles updates off its frame thread. The
server still owns account eligibility and the durable session move. The client owns rendering and saving its scale.

## The window and conversation applications

The person’s messages align right and the friend’s replies align left, including while streaming. Set
`transcript_style` in the client’s `config.json`, or return it from the Starlark `window` event to vary the
appearance by tab. The shipped profile uses these defaults:

```json
{
  "transcript_style": {
    "fill": "#252525",
    "ink": "#f3f3f3",
    "notice_ink": "#ffb4b4",
    "status_ink": "#d8d8d8",
    "radius": 25.2,
    "show_notices": true,
    "show_tools": true
  }
}
```

`fill` and `ink` set the conversation bubble background and text. Set `person_ink` to give sent and queued
messages their own text color. Omitting it uses `ink` for both speakers. `notice_ink` sets the notice color and
`status_ink` sets the status text color. Colors accept six hexadecimal digits after `#`. The corner radius
accepts 0–64 logical points before keyboard zoom. All fields are optional. Set `show_notices` or `show_tools`
to `false` to hide those transcript entries in the client. Their history stays available when you enable them
again. Apply the client policy to see the changes in a running window.

When the selected account needs sign-in, the shipped client policy shows a centered sign-in card and the composer.
A client profile can customize this card by setting `message_card` in
`config.json` or returning it from `app.window`. Its defaults are
`{"fill":"#ffffff","ink":"#000000","email_ink":"#477fba","text_size":34,"radius":20}`. Colors are opaque `#RRGGBB`.
The text size (12–72 points) and corner radius (0–64 points) scale with keyboard zoom.
The centered card shows the selected account’s label (or account name), its email in `email_ink` on the next
line when the server supplies one, and the keyboard instruction below. Clicking the card or pressing Enter in
the prompt opens that account’s login. The client `window` event receives `sign_in_account` as an object with
string fields `name`, `label`, and `email` while sign-in is available, and `null` otherwise. Unspecified
labels and emails are empty strings.

Preview application events with an `app.` prefix. The Starlark `handle` function receives the event name without
the prefix. For example:

```sh
echo '{"tab":"friend","type":"","prompt_open":true,"has_status":false}' |
  build/nexus-policy preview client policy/rules app.window
echo '{"friend":"reviewer","intent":"send","text":"hello","model":"","queued":0,"busy":false}' |
  build/nexus-policy preview server policy/rules app.message
```

| Event | State and return value |
|---|---|
| Client `window` | Receives the active tab kind, registered type ID, prompt availability and presence of link status. Returns the ordered panel list, keyboard shortcuts, and optional tab registrations and label. The window caches this description until its state changes or it observes an applied revision during a frame. |
| Client `prompt_key` | Receives `key` (`Enter` or `Command+Enter`), `text`, `friend`, `model`, queue length `queued`, and displayed `busy` state. Returns a command list. |
| Server `message` | Receives the same text and session fields with `intent` (`send` or `queue`) and server-side `busy` (an open turn). Returns a command list. |
| Server `new_session` | Receives `friend`, driver `defaults`, an optional `source` session, and an optional `roster` with `linked`, `preferred` and `instances`. Returns string fields `account`, `instance`, `model` and `effort`. The host validates these before creating the session. |

Message command lists accept up to 16 operations. Each operation is a dictionary with an `op` field. A `notice`
carries `text`. A client `draft` replaces the current draft. A final `send` or `queue` can supply replacement
`text` or use the input. A final server `native` names a registered service in `name` and can supply input
`text`. An empty list consumes the input. The server validates the entire plan before acting and commits its
changes together with the request acknowledgement. Stop, credential input and sign-in take dedicated engine paths.

The shipped modules are `client/window.star`, `server/conversation.star` and `server/sessions.star`. Add modules in
your fork, import them from an entry point, and check the imports in with the config. For a complete registration
and both native and script handlers, see [tab and service plugins](plugins.md).

## Claude driver startup

The server's Claude driver launcher asks `driver_start(context, config)` at each driver start, including a resume.
The policy and data live together in a directory chosen by the person. The launcher only reads that directory.
A minimal profile is in [policy/example](../policy/example).

| Input | Meaning |
|---|---|
| `context["friend"]` | The profile name assigned to this driver binding |
| `context["provider"]` | `claude` for this hook |
| `context["account"]` | The account name selected by the server |
| `context["resuming"]` | Whether the server is passing a prior harness session to the driver |
| `config` | The JSON object from the profile's `config.json` |

Return a dictionary. An empty dictionary uses the driver's defaults. The optional `model` is a model identifier
of up to 64 characters. `effort` is one of `low`, `medium`, `high`, `xhigh` or `max`. An explicit driver flag
wins over a profile default. The server continues to send explicit session settings over the driver protocol.

For example, this policy uses less effort for a fresh conversation with a designated friend. The person supplies
`quick_friend` in their own JSON.

```python
def driver_start(context, config):
    if context["friend"] == config.get("quick_friend") and not context["resuming"]:
        return {"effort": "low"}
    return {}
```

The interpreter receives frozen data, with a step and allocation budget, in a separate process with a three-second
deadline. The hook has no host I/O functions or module loader. A policy error produces a driver error with a reason
and leaves the prompt unsent to the provider. Correct the profile, preview it, and retry. A change takes effect on
the next start. A driver already answering a turn continues with its current settings.

## Friend identity at startup

A server profile can keep `identities.json` next to `policy.star`. The launcher queries
`nova-config friend show NAME --json` at each driver start. The registry supplies the friend's name,
harness, model, pronouns, note, enabled state and `startup` object. Its `home` is the absolute checkout
of the named `repository`, its `prefix` holds the opening instructions, and `files` lists home documents
in reading order. Set them together with `nova-config friend set NAME --startup '<json>' --actor NAME`.
The profile only selects the registry command, for example:

```json
{"nova_config": ["/usr/local/bin/nova-config"]}
```

The home documents must be regular files tracked at HEAD. The launcher reads their working-copy text,
including local edits. It records the base commit and a digest of the exact startup context. Use a
read-only registry connection. The optional arguments following the `nova_config` program can select
its store. Keep credentials out of the profile.

The server substitutes `{friend}` and `{harness}` when either occupies an entire driver argument.
For example, one command can serve all friends using an installed Claude adapter:

```text
driver claude /opt/nexus/venv/bin/python -I -m nexus_policy friend-driver /srv/nexus/profile {friend} {harness} /usr/bin/node /opt/nexus/drivers/claude/dist/src/main.js --account work
```

The launcher verifies the selected harness against the registry before running the driver. The configured
model is a default. A registry model equal to the harness name uses the harness default.
An explicit session setting takes precedence. The `driver_start` hook also receives `context["identity"]`
with the friend, harness, registry model, home path, files, revision and digest.
The CLI can verify a definition and report this metadata without starting a model:

```sh
/opt/nexus/venv/bin/python -I -m nexus_policy identity /srv/nexus/profile reviewer --harness claude
```

The adapter receives the startup context in an inherited temporary file descriptor
named by `--startup-context-fd`. It consumes and closes the descriptor before launching the model.
The shipped Claude adapter appends the text to the Claude Code system prompt on new and resumed queries.
Codex, Antigravity, Grok and Muse consume the same descriptor through the [RPC adapters](../drivers/rpc/README.md).
An identity profile alone does not install a driver or add a friend to the roster.

## Install and use

On the server, install the policy host once with Python 3.11 or newer. From the Nexus checkout:

```sh
runtime="$HOME/.nexus/server/policy-runtime"
python3 -m venv "$runtime"
"$runtime/bin/python" -I -m pip install ./policy
profile="$PWD/policy/example"
"$runtime/bin/python" -I -m nexus_policy check "$profile" reviewer --account work
"$runtime/bin/python" -I -m nexus_policy check "$profile" reviewer --account work --resuming
```

In the existing [drivers file](../drivers/claude/drivers.example), place the policy host before the Claude driver
command. Use absolute paths without spaces in this file. For example:

```text
driver claude /home/me/.nexus/server/policy-runtime/bin/python -I -m nexus_policy driver /home/me/nexus/policy/example reviewer /usr/bin/node /home/me/.nexus/server/current/drivers/claude/dist/src/main.js --account work
friend reviewer claude ~/work
```

The label after the profile path selects the friend's policy. Give friends with different labels separate driver
lines. Keep the account links, billing and instances in [the server's configuration](accounts.md).

Keep the driver’s `policy.star` and `config.json` together in your fork. Choose a working directory your configuring AI
already has permission to edit. Preview changes with `check` and replace files atomically when updating a live
profile. To move to another server, copy those two files, point its driver binding at the profile and connect its
own account. The policy host needs installing only once per server.

To carry the friend roster too, copy the applied `accounts.json` and `friends.json` from the directory reported by
`nexus-server config path` into your profile. These files contain the account names and links. On a new machine,
adapt the instance machine and directory, install the driver binding, then use the normal `config check` and
`config reload` workflow for the two files. An exported roster is a snapshot. Changes to those two copies take
effect when applied to the server configuration. Each machine signs in through Nexus.
