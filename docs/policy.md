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

## Start with the keyboard-driven client

Copy `policy/rules/` into your own profile directory. Use
[`keyboard-client.json`](../policy/examples/keyboard-client.json) as that copy's
`config.json` to start with the dark tab strip, fixed-width sign-in dialog, keyboard
account picker and matching prompt. The example contains no account or friend identities.
Edit the JSON for visual changes. Edit `client/window.star` for tab titles, panels, prompt
commands and shortcuts, and `client/accounts.star` for sign-in visibility, selection
and dismissal. Apply only the client profile with the installed tool.

```sh
nexus-policy apply client /path/to/my-profile
```

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

With `centered_account_menu` enabled, friend tabs show only the friend’s name. An opening arrow
highlights the current account in server order. When sign-in is required, the white sign-in card already
contains the selector. Arrows move immediately and Enter selects the highlighted account. Escape dismisses
the reminder and focuses the draft. Pressing Enter on the draft while signed out brings the reminder back
and keeps the text for later.

The live Nova Tools directory supplies provider display names. On Studio a login service runs
`nova-config friend watch --output ~/.nexus/client/nova-friends.json` with the fleet’s read-only
database connection. It publishes the current friends and provider labels on connection and on committed
registry edits. The client policy receives the snapshot as `config["nova_friends"]`. Set a shared
label with `nova-config harness set codex --display_name ChatGPT`. The account’s provider identifies
the service the user will authenticate with. Each row shows the configured email, falling back to
the account label if the roster has no email. Bind `ArrowUp` and `ArrowDown` in
`account_picker_shortcuts` to use unmodified arrows from the prompt. Enable `sign_in_on_select`
to start sign-in when the server acknowledges a different account that needs authentication.

### Editing sign-in and tabs in Starlark

The window calls `app.window` with the active tab, sign-in state, limit offer and `ui`.
The latter includes ordered `tabs` (kind, label, type and group), linked `accounts` (name and label),
`current`, `highlighted`, `dismissed` and the local `draft`. `client/accounts.star` uses those facts
to compose the personal account surface. Returning `account_items` gives the fork control over
prompt and sign-in gestures. Use an empty list to hide the surface or omit the account panel.
Order text and buttons freely. A text item has `text`. A button also has an `action` and may
set `selected`. Both buttons and shortcuts can request the same host operations.

| Operation | Effect |
| --- | --- |
| `focus:ID` | Preview a linked account locally. |
| `select:ID` | Select a linked account for the session. |
| `sign_in:ID` | Select the account, wait for server confirmation, and request sign-in if needed. |
| `dismiss` | Clear the preview and remember dismissal for the active tab and account. |
| `reveal` | Clear dismissal so the policy can show the reminder again. |

For example, a fork can show a single “Continue” button and bind its action to Command-K.

```python
action = "sign_in:" + state["ui"]["current"]
window["panels"] = ["account", "prompt"]
window["account_items"] = [{"text": "Continue", "action": action}]
window["shortcuts"].append(["Command+K", "account_action", action])
```

The returned `message_card` controls heading size, card fill, radius, `selected_fill`, `selected_ink`
and `email_weight`. Set `row_height`, `row_gap` and `heading_gap` in multiples of the active body
line height. `selector_width_em` fixes the selector width in body-font units (default 16).
`vertical_extension` adds a percentage of the content height above and below the card (default 10).
`selected_opacity` sets the highlight alpha (default 0.5). `selected_radius` sets its corner radius.
`unselected_weight` sets the ordinary account font weight.
`side_extension` adds a percentage of the content card’s width on each side, from 0 to 50.
Only the background grows.

The client Starlark window may return a `tab_style` object. Set `fill`, `opacity`, `ink` and
`weight` for the strip and its ordinary titles. Set `selected_fill`, `selected_opacity`,
`selected_radius`, `selected_ink` and `selected_weight` for the active tab. Colors use
`#RRGGBB`, opacity ranges from 0 to 1, radii from 0 to 64, and font weights from 100 to 900.
Set `height`, `tab_height`, `leading` and `gap` for strip height, pill height, leading space and space between tabs.
With `reserve_width` enabled, the renderer measures both weights and reserves the wider title.

Return `prompt_style` from the window event to set `fill`, `border`, `ink`, `selection`, `width`
(percent), `radius`, `height`, `padding_x`, `gap`, `max_lines` and `show_plus`.
Point sizes follow the user's zoom. The stock client reads these objects from its config.
For the global overlay shortcut, the client handles an independent `overlay` event even
while hidden. Return a physical key code and modifiers, for example
`{"code":"KeyJ","modifiers":"Command+Shift"}`. The stock rule reads `overlay_hotkey`.
Use UI Events physical codes such as `Backquote`, `KeyJ` and `F8`. In-window shortcuts
continue to use logical key names. Applying a client profile updates the global binding.

The fork can return `prompt_command` for a draft that should bypass the account reminder.
The prompt handler receives `signed_out` and can return `clear_local` to clear the active
friend's visible history and draft. The window can bind a key to `["replace_draft", text]`.
The stock Tab binding derives its completions from `ui.catalog.models` and `ui.model`.

Return `tab_titles` in current tab order to label tabs, `show_tabs` to show the strip and
`reorder_tabs` to allow dragging. The same window description selects tab groups, plugin types,
panels and shortcuts. Apply the edited rules with `nexus-policy apply client RULES`. The running
window adopts the new program at a frame boundary.

An existing `account_shortcuts` map still sends its configured `account_command` actions immediately. For example,
`"account_shortcuts": {"Command+ArrowDown": "advance"}` asks the server’s
`decide("account_command", context, config)` to choose an account. Server config
`"account_navigation": {"steps": {"advance": 1}, "wrap": false}` stops at the final account. If a profile specifies
both maps for the same chord, `account_picker_shortcuts` takes precedence.

The stock server profile sets `"auto_accounts": true`. Fresh conversations with at least two linked
subscriptions start in Auto. The first signed-in subscription pays, or the first linked subscription supplies
the sign-in card. A same-friend copy inherits the source selection mode. Named defaults follow
`account_default`, either `"first"` (stock) or `"preferred"`. Forks can set `auto_accounts` to false to use
named defaults for every new session.

## Fable limit choices

When the provider reports a Fable-specific limit, the friend tab keeps the transcript visible and holds its draft while you choose how to continue. Press Command-Shift-A to switch to the offered account or Command-Shift-O to use the offered model on the current account. Each button also responds to a click. The server supplies the available choices and validates the choice against the interrupted turn.

In the client's `window` event, `limit_offer` is present for an interrupted turn. It contains the proposed `account` and `model` (empty when unavailable), plus `connected` and `waiting`. The stock policy renders the `limit_choice` panel using `label` for the question and `limit_account_label` and `limit_model_label` for the buttons. Change the wording in the fork's `client/window.star`. Set `limit_choice_keys` in the client configuration to customize the bindings, for example:

```json
{"limit_choice_keys": {"account": "Command+Shift+A", "model": "Command+Shift+O"}}
```

## Events available in the native hosts

| Host and event | Context | Result |
|---|---|---|
| Client `text_zoom` | Integer `current` text scale step, signed key `delta`, inclusive `min` and `max` supported by the renderer | An integer in range. On an invalid answer the window keeps the current scale and reports the error. |
| Server `account_after_limit` | `current` account name, `linked` names in configuration order, and `eligible` names of other signed-in linked subscriptions that have not exhausted this request | An eligible name, or `None` to stay. An invalid answer keeps the session on its current account and adds a notice. |
| Server `model_after_limit` | `friend`, concrete `account`, current `model` after a Claude Fable limit | The model to offer on the same account, or `None` to omit this choice. Stock policy reads `fable_fallback_model`, defaulting to the Claude `opus` alias. The server resumes only when the person selects an available option. |

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
    "switch_email_ink": "#86c5ff",
    "radius": 25.2,
    "show_notices": true,
    "show_tools": true
  }
}
```

`fill` colors the sender’s bubble and `ink` colors the reply text. The conversation panel uses the tab strip fill and the prompt width. Set `reply_width` to a percentage from 1 to 100 (default two thirds). Set `conversation_panel` or `markdown` to false to disable the panel or Markdown rendering. Set `person_ink` to give sent and queued
messages their own text color. Omitting it uses `ink` for both speakers. `notice_ink` sets the notice color and
`status_ink` sets the status text color, including the words around the address in an account-switch notice.
`switch_email_ink` sets the address color in that notice. Colors accept six hexadecimal digits after `#`. The corner radius
accepts 0–64 logical points before keyboard zoom. All fields are optional. Set `show_notices` or `show_tools`
to `false` to hide those transcript entries in the client. Their history stays available when you enable them
again. In `transcript_style`, `"tool_prompts": {"Bash": "$ "}` displays Bash activity as a selectable monospace command. Each key matches the tool name exactly. An empty map preserves the original tool labels. Apply the client policy to see the changes in a running window.

The Starlark `window` result accepts these color settings, also available in the client JSON profile:

| Setting | Surface |
| --- | --- |
| `transcript_style.ink` | AI reply text |
| `transcript_style.person_ink` | Sent and queued message text |
| `transcript_style.fill` | Sent message bubbles and tool blocks |
| `transcript_style.panel_fill` | Output panel (defaults to the tab strip fill) |
| `transcript_style.command_ink` | AI tool activity, including shell commands |
| `transcript_style.link_ink` | HTTP and HTTPS links (defaults to the selected tab fill) |
| `transcript_style.code_fill` | Rounded background behind inline code |
| `transcript_style.code_ink` | Inline code text (defaults to the speaker’s color) |
| `prompt_style.fill` | Prompt input background |
| `prompt_style.ink` | Typed draft text |

Inline code uses the monospace font. `code_size_offset` adjusts its size in logical points relative to the monospace base size (−8 to 32, default +2). `code_padding` sets horizontal padding in logical points (default 6), `code_radius` sets the corner radius (default 7), and `code_background` toggles the fill. These fields belong to `transcript_style` and scale with keyboard zoom.

Markdown list items share a marker column. Set `transcript_style.list_gap_em` to change the space between the marker and the text at every zoom level (0–4, default 0.4).

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
| Server `new_session` | Receives `friend`, driver `defaults`, an optional `source` session, and an optional `roster` with `linked`, `preferred`, `instances` and `can_auto_account`. Returns optional boolean `auto_account` (default false) and string fields `account`, `instance`, `model` and `effort`. The host validates these before creating the session. |

Message command lists accept up to 16 operations. Each operation is a dictionary with an `op` field. A `notice`
carries `text`. A client `draft` replaces the current draft. A final `send` or `queue` can supply replacement
`text` or use the input. A final server `native` names a registered service in `name` and can supply input
`text`. An empty list consumes the input. The server validates the entire plan before acting and commits its
changes together with the request acknowledgement. The client accepts a single `sign_out` operation for Enter on
the exact draft `/logout`. It targets the account the server confirmed for this conversation. The command stays in the
window when queued or unsupported. Claude sign-out removes the selected Nexus login and returns the friend tab to its sign-in card. An active turn or browser sign-in must finish first. Stop, credential input and sign-in take dedicated engine paths.

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


### Conversation commands

The stock server message handler implements `/model`, `/effort`, `/compact` and `/clear`.
The first two list choices or accept a value from the session's harness catalog. Switching
models resets the effort to the new model's default. Clearing retains the friend, account
and model settings. Compaction releases the provider session and starts the next turn with
Nexus's bounded transcript handoff. Both context operations require an idle conversation.
When signed out, the stock client clears its own visible messages on `/clear`.

### Conversation commands and the activity indicator

The server handles `/info` and `/model [model [effort]]` in the conversation Starlark module.
A combined selection returns one `set_model` operation with `text` and an optional `effort`.
Both settings share a durable commit. Submitting bare `/model` or `/effort` in the window
opens the centered selector. Arrow keys move the highlight, Enter applies it and Escape restores
the prompt. The catalog's `order` array preserves harness discovery order across JSON and Starlark.
The first listed model is the startup choice when the session has no explicit model. Model identifiers remain the harness's own identifiers.
When the harness supplies a concrete target for a rolling alias, the model list and session info show it.
The prompt's Tab binding completes effort against the model typed in the draft.
The `/info` notice includes the selected friend, optional configured pronouns, account email and sign-in state, model,
effort, home repository and working directories. The friend-driver wrapper publishes public launch
metadata from nova-config and its launch arguments. An unavailable field is identified in the notice.

The prompt insertion point is a rounded orange segment. Set `prompt_style.caret_ink` for its color and
`prompt_style.caret_peak` (1–8, default 3) for its HDR peak. Its width is `prompt_style.caret_aspect` times its height (default 0.618034). Positive `caret_offset_y` lowers the cursor in points before zoom. Set `caret_height` (1–28 points before zoom), `caret_radius` (0–32 points) and `caret_seconds` (0.4–10 seconds per pulse) in the same object.
The cursor rises for two thirds of the moving part of each cycle, holds at both maximum and minimum brightness for `caret_hold` seconds each (default 0.1, capped at 45% of the cycle per hold), and fades over `caret_fall_ratio` times the rise duration (default 0.5).
The scanner reads the same height, aspect and corner radius. Both start at the prompt’s typing inset.
Return these settings in `prompt_style` from the Starlark `window` event, or set them in the client profile and apply it. Reduced motion keeps the cursor steady.

The activity strip uses the tab selection color unless `transcript_style.scanner_ink` supplies another.
Each segment uses the same dimensions as the prompt cursor. Each active agent gets one lit cell, including the friend during its own turn. Long strips show
the remaining count numerically. `scanner_gap` sets the space between cells in points before zoom. `scanner_offset` shifts the strip horizontally from the prompt’s starting column (default 0).
`highlight_seconds` sets the duration of a complete selection pulse (default 4 seconds). `flash_seconds` sets the selection flash’s exponential decay time (default 0.22 seconds). Idle settles to one cell in the prompt fill. Set `scanner_seconds`
(0.4–10, default 1.5) for a full sweep, `scanner_peak` (1–8, default 3) for peak linear light,
and `highlight_energy` (0–4, shipped setting 2.5) for the selection flash and pulse on tabs and in selectors. Reduced motion
disables the sweep and collapse. Both the overlay and opaque windows request HDR. Prax preserves transparency and falls back to SDR on displays without HDR.

### Editing motion in the client profile

Sent bubbles use a solid white 3-point outline, set by `transcript_style.sent_outline_width` and `transcript_style.sent_outline_ink`. Pending bubbles use a dashed selection-color outline. Set `transcript_style.pending_outline_width` from 0 to 8 points before text scaling (default 3). `sounds.startup` plays once the overlay hotkey registers. Set `synth` to true for a layered tone that swells and fades. `sounds.step` is the quiet tick when the selected menu row changes. `sounds.hold` plays on staging (silent by default), and `sounds.send` plays when a new outgoing message appears in the live server transcript.

Read tool calls display the path alone. `transcript_style.read_path_ink` sets its color and defaults to the link or selection color.

For a single active cell, `transcript_style.scanner_pulse_floor` sets the dimmest part of the pulse (0–1, default 0.12).

The Starlark window policy builds the cursor and scanner curves. Set `prompt_style.caret_bottom_hold` independently to change how long the cursor stays dim. It inherits `caret_hold` when omitted. For a different animation, supply `prompt_style.caret_curve` or `transcript_style.scanner_curve` directly. Each is a list of 2–64 `[time, value]` pairs between 0 and 1. Start at time 0 and end at 1, in increasing order. Equal successive values create a hold. The renderer eases between values. For the cursor the value controls brightness, and for the scanner it controls the position along the row.

```python
# Pause at both ends of the scanner sweep.
style["scanner_curve"] = [[0, 0], [0.1, 0], [0.5, 1], [0.6, 1], [1, 0]]
```

The scanner takes `scanner_pause` seconds before collapsing, uses `scanner_ease` as its expansion and collapse time constant, and fades its last light over `scanner_fade` seconds. Apply the client profile to update a running window.

Set `prompt_style.caret_gap` to the trailing space in points between typed text and the prompt cursor. The keyboard profile uses 2 points.

The window policy can return a `sounds` object with `tab` and `confirm` cues. Each has `from_hz`, `to_hz`, `duration_ms`, and `gain` (0–1). Omit a cue or set its gain to zero to silence it.

`prompt_style.text_peak` sets steady HDR brightness for the entire draft, capped by display headroom. The keyboard profile uses `prompt_style.bold` for the editor and `transcript_style.person_bold` for the person's messages. In `transcript_style`, `person_flash_peak` and `person_flash_seconds` set the initial brightness and fade of a submitted message. `glyph_peak`, `glyph_seconds`, and `glyph_stagger` control the short-lived glow of newly streamed reply text. Peak brightness is a linear-light multiplier capped by the display's available HDR headroom. A peak of 1 disables the corresponding effect. Reduced Motion disables both.

The client profile's `clear_draft_keys` binds the `clear_composer` action, which clears the active friend's draft and all its locally staged bubbles in one press. The defaults cover Command with either the Mac Delete key (Backspace) or forward Delete.

Set `hold_on_enter` for client-side message staging. Enter holds the current draft in memory for that tab. Empty Enter releases the batch in order. Command-Enter adds the current draft last and releases the batch. Messages reach the server only on release. Slash commands execute directly.

The optional `sounds` entries `open`, `close`, `dialog`, `confirm`, `cancel`, and `ready` use the same chirp fields. The client remembers /quiet across launches. /quiet off and /loud restore sound.
