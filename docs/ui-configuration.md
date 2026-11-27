# UI configuration

Edit `policy/rules/config.json` and the Starlark modules in your fork. This is the settings interface: ask your AI to
change the profile and keep the change in git. The client renders the validated result of `client/window.star`;
`server/conversation.star` owns command presentation. No settings screen or C++ edit is needed for these choices.

## Applying a profile

Build `nexus-policy`, then run `nexus-policy apply client policy/rules <client-home>` (and `apply server` to the
server home for command output changes). Apply to a staging directory to prepare an update without changing a live
session. The client watches its bundle and adopts valid appearance changes at a frame boundary, rebuilding cached
fonts and geometry. Native validation rejects invalid keys, types, colors and unsafe numeric values. A bad window
result reports its cause and uses the packaged profile. Keep a previous bundle to restore a customization.

OS window creation options (`overlay_blur`, `overlay_accessory`, initial `window_width` and `window_height`) take
effect on the next launch. The overlay hotkey is also registered at launch. Typography, colors, copy, clock, keys,
scroll tuning and animation settings refresh in the running window. No server restart is needed for client styling.

## Shipped appearance

The header above the divider is opaque black (`tab_style.fill: "#000000"`, `opacity: 1`), including the clock
background, so it blends into a Mac display notch.

The outer AI-chat canvas is sRGB `#202020` with `overlay_opacity: 0.9` (10% transparency). The output panel uses
`transcript_style.panel_fill: "#272728"` at full opacity. Prompt, user bubbles, work bubbles and app information
bubbles retain independently configurable fills. An ordinary window remains opaque.

App information is a compact left-aligned Markdown bubble with white text. Routine account/sign-in chatter is hidden
by `transcript_style.hidden_notice_fragments`, without removing durable history. Actionable failures remain visible.
File tools use `#aa79dd`, with almost the same OKLCH lightness as shell orange `#dc7334` (approximately 0.666).
Tool names map to colors and prefix replacements through `tool_inks` and `tool_prompts`. Work bubbles have a
noninteractive top-left chevron; `tool_chevron` turns it off and the size, gap and ink fields style it.

The active goal has its own pinned bubble below the held message and above the prompt. Its label, outline width,
gap and height limits are in `transcript_style`. Its orange outline uses the cursor's pulse curve and HDR headroom.
Both `/goal clear` and `/goal off` stop it; Command–Delete also clears the composer. See [goals](goals.md).

## Clock

`clock.visible` controls the local date/time above the top-right divider. The default format is
`{weekday} {day} {month} {hour12}:{minute} {ampm}`, for example **Fri 27 Nov 10:45 AM**. It uses the client's local
calendar, including timezone and daylight-saving changes. `{year}`, `{hour24}` and `{second}` are also supported.
Weekday labels start on Sunday; supply seven `weekdays` and twelve `months` labels to localize them, with `am` and
`pm`. The host samples time once a second and when drawing; it does not request frames while the desktop is hidden.
Clock size/margins/vertical offset are in `ui_style`; `ui_colors.clock_ink` sets its color. `show_tabs: false` hides the
whole header, including the clock.

`ui_flags.notch_aware` defaults to true. On macOS, the host measures the current display's camera housing from
`NSScreen.safeAreaInsets` and its auxiliary top-left/right areas, converted into this window's logical coordinates.
Only an actual overlap reserves space; ordinary windows below the menu bar and displays without a notch use the
normal header. The measurement refreshes each frame, including after a display change. The host exposes this
rectangle as `ui.notch` (`left`, `top`, `right`, `bottom`) to `app.window`.

Tabs scroll horizontally within the left safe area; selecting a tab reveals it. The clock occupies the right safe
area. `ui_style.notch_margin` (12 logical points, scaled with text) keeps content away from the housing. If either
side is narrower than `ui_style.notch_min_lane_width` (280 points, scaled), the whole header goes below the notch.
The header remains at least as tall as the obstruction, so content below the divider is unobscured too.
Set `notch_aware` to false to restore the unrestricted header. This changes on policy reload.

## Animation and scheduling

`transcript_style.idle_animations: true` keeps pulses running while idle and on battery. Set it to false for steady
idle visuals, or set `battery_animation_idle_seconds` to a positive timeout (0 means continuous). Reduced Motion is
always honored. Held-text pulse amount/period, cursor/scanner curves, HDR peaks retain their
individual settings in `prompt_style` and `transcript_style`. New AI text has no HDR flash by default (`glyph_peak: 1`).

The macOS client assigns itself the foreground-application role, raises its UI thread to user-interactive QoS and
holds a user-interactive activity while in front. Hiding the window releases the boost. Set
`ui_flags.foreground_priority: false` to disable this behavior. The shipped LaunchAgent also uses
`ProcessType: Interactive` and `Nice: -20`, the highest normal scheduling priority; this is editable deployment
configuration in `macos/com.robotgamedevelopers.nexus-client.plist`. OS permissions govern whether a requested nice
value is granted. It takes effect when the service next launches; replacing the plist does not change a running
process. Compile and test jobs should run at low priority (`nice -n 15`) so interaction wins during development.

## Configuration groups

| Group | Controls |
|---|---|
| `ui_style` | Logical-point geometry, typography, zoom, selection, cursor state colors in OKLCH, initial window and clock sizing |
| `ui_colors` | Canvas, control and clock colors (`#RRGGBB`) |
| `ui_palette` | Optional overrides of Prax control palette slots (`#RRGGBB` or `#RRGGBBAA`) |
| `ui_text` | Native UI captions and named-placeholder templates; inserted values are never interpreted as templates |
| `ui_keys` | Native prompt submit/queue/paste and dialog navigation chords; empty disables a chord |
| `ui_flags` | Overlay and mobile presentation choices |
| `ui_fonts` | `ui` and `mono` font family names; empty uses the system default |
| `ui_scroll` | Board scrolling dynamics, settling thresholds and timeouts |
| `clock` | Date/time visibility, format and localized labels |
| `prompt_style` | Prompt dimensions, fill, selection, caret and HDR text |
| `transcript_style` | Reply/notice/tool/goal presentation, Markdown, filtering, effects and idle animation |
| `tab_style`, `account_menu`, `message_card` | Tab strip, account selector and message-card presentation |
| `sounds` | Named sound frequencies, durations, gains and synthesis; `/quiet` mutes all |

`ui_palette` accepts `accent`, `accentWeak`, `textSelection`, `accentText`, `accentHover`, `accentPress`, `success`,
`warning`, `danger`, `bg0`…`bg4`, `bgViewport`, `line1`…`line3`, `text1`…`text4`, `textInverse`, `hudFill`, `hudBorder`,
and `axisX`…`axisW`. Overrides apply after the Nexus theme. Terminal fonts use `ui_fonts.mono` and
`ui_style.terminal_size`; terminal application escape sequences still control their own colors.

Sizes are logical points before zoom unless the name says percent, em, lines, scale, ratio or fraction. Font weights
are numeric. Cursor hue is degrees; lightness/chroma are OKLCH components. `ui_scroll` durations are seconds except
`coast_decay_per_ms`; speeds are pixels/second. Numeric settings must be finite. Geometry is bounded to 0–4096,
with positive body size/line pitches and safe percentage/zoom limits. Scroll values are positive and at most 10000;
`coast_decay_per_ms` must be below 1. Unknown `ui_*` fields are refused. Caption templates are at most 4096 bytes;
formatted clock text is at most 256 bytes.

The higher-level binding maps (`tab_navigation`, `tab_reorder`, `account_picker_keys`, `account_shortcuts`,
`transcript_scroll_keys`, `clear_draft_keys`, etc.) remain Starlark-configurable. `/help` documents the shipped keys
and uses the configured submit/queue and binding maps. Forks can extend command descriptions with `command_help`.
Prax's ordinary text-editing keys and OS-reserved shortcuts remain platform behavior.

Native code retains rendering algorithms, protocol/resource limits, privacy filtering and state validation.
These are implementation invariants, not presentation defaults. CMake generates `nexus_ui_defaults.inc` from the
shipped JSON; do not edit the generated file. The following tables inventory the new profile fields; the JSON is
canonical, and `docs/policy.md` documents the existing style groups.

## ui_style

| Field | Shipped default |
|---|---|
| `body_size` | `28` |
| `label_size` | `24` |
| `meta_size` | `21` |
| `terminal_size` | `21` |
| `line_pitch` | `39.76` |
| `entry_gap` | `44.8` |
| `bubble_padding_x` | `29.96` |
| `bubble_padding_y` | `21` |
| `bubble_max_percent` | `83` |
| `person_bubble_max_percent` | `66.666667` |
| `plus_size` | `36.4` |
| `column_inset` | `16.8` |
| `tool_size` | `21.84` |
| `tool_pitch` | `28.392` |
| `cursor_gap` | `14` |
| `footer_padding` | `28` |
| `action_width` | `80` |
| `zoom_step` | `0.1` |
| `zoom_min` | `0.5` |
| `zoom_max` | `2` |
| `default_zoom_desktop` | `1` |
| `default_zoom_mobile` | `0.8` |
| `regular_width_column` | `0.9` |
| `title_scale` | `1.35` |
| `heading_scale` | `1.15` |
| `small_line_height` | `1.2` |
| `meta_line_height` | `1.3` |
| `display_line_height` | `1.42` |
| `normal_weight` | `400` |
| `heading_weight` | `700` |
| `letter_spacing` | `0` |
| `list_column_em` | `0.7` |
| `code_line_height` | `1.3` |
| `prompt_border_width` | `1` |
| `insertion_caret_width` | `2.5` |
| `insertion_caret_min_width` | `2` |
| `caret_floor` | `0.48` |
| `selection_opacity` | `0.42` |
| `transcript_selection_opacity` | `0.65` |
| `focused_outline_width` | `2` |
| `focused_outline_inset` | `4` |
| `selector_padding_lines` | `0.18` |
| `tab_divider_height` | `2` |
| `strip_border_height` | `1` |
| `pending_dash_length` | `3` |
| `pending_dash_gap` | `2` |
| `scanner_dim_gain` | `0.09` |
| `scanner_spread` | `0.55` |
| `account_chevron_scale` | `0.7` |
| `account_anchor_extra_height` | `4` |
| `dialog_margin` | `60` |
| `scroll_page_fraction` | `0.9` |
| `dialog_width` | `420` |
| `dialog_inset` | `120` |
| `account_row_extra_height` | `8` |
| `highlight_sdr_pulse` | `0.06` |
| `highlight_sdr_flash` | `0.19` |
| `highlight_hdr_floor` | `0.1` |
| `highlight_hdr_pulse` | `0.04` |
| `highlight_hdr_flash` | `1.25` |
| `glyph_birth_spacing` | `0.008` |
| `selection_pulse_amplitude` | `0.25` |
| `window_width` | `1000` |
| `window_height` | `780` |
| `clock_size` | `28` |
| `clock_margin_right` | `22.4` |
| `clock_margin_left` | `16` |
| `notch_margin` | `12` |
| `notch_min_lane_width` | `280` |
| `clock_offset_y` | `0` |
| `bold_weight` | `700` |
| `cursor_busy_dim_lightness` | `0.66` |
| `cursor_busy_dim_chroma` | `0.1` |
| `cursor_busy_dim_hue` | `235` |
| `cursor_busy_bright_lightness` | `0.86` |
| `cursor_busy_bright_chroma` | `0.1` |
| `cursor_busy_bright_hue` | `235` |
| `cursor_working_lightness` | `0.76` |
| `cursor_working_chroma` | `0.16` |
| `cursor_working_hue` | `62` |
| `cursor_idle_lightness` | `0.5` |
| `cursor_idle_chroma` | `0.15` |
| `cursor_idle_hue` | `262` |
| `cursor_down_lightness` | `0.62` |
| `cursor_down_chroma` | `0.21` |
| `cursor_down_hue` | `27` |
| `cursor_lost_lightness` | `0.42` |
| `cursor_lost_chroma` | `0` |
| `cursor_lost_hue` | `0` |

## ui_colors

| Field | Shipped default |
|---|---|
| `background` | `"#202020"` |
| `control_hover` | `"#303030"` |
| `control_divider` | `"#373737"` |
| `control_ink` | `"#f3f3f3"` |
| `control_muted_ink` | `"#9e9e9e"` |
| `control_faint_ink` | `"#8c8c8c"` |
| `clock_ink` | `"#f3f3f3"` |

## ui_keys

| Field | Shipped default |
|---|---|
| `submit` | `"Enter"` |
| `queue` | `"Command+Enter"` |
| `paste` | `"Command+V"` |
| `confirm` | `"Enter"` |
| `dismiss` | `"Escape"` |
| `previous` | `"ArrowUp"` |
| `next` | `"ArrowDown"` |

## ui_flags

| Field | Shipped default |
|---|---|
| `foreground_priority` | `true` |
| `notch_aware` | `true` |
| `performance_recording` | `true` |
| `overlay_blur` | `false` |
| `overlay_accessory` | `true` |
| `hide_on_blur` | `true` |
| `protect_overlay_focus` | `true` |
| `dashboard_background_updates` | `true` |
| `opaque_history_backdrop` | `true` |
| `mobile_top_dialogs` | `true` |
| `mobile_tap_ends_editing` | `true` |
| `mobile_full_width_replies` | `true` |

## ui_fonts

| Field | Shipped default |
|---|---|
| `ui` | `""` |
| `mono` | `""` |

## ui_scroll

| Field | Shipped default |
|---|---|
| `coast_decay_per_ms` | `0.99` |
| `hold_pixels_per_second` | `2000` |
| `hold_tap_seconds` | `0.08` |
| `spring_omega` | `16` |
| `bump_pixels` | `24` |
| `rubber_band_coefficient` | `0.55` |
| `settle_pixels` | `0.5` |
| `settle_speed` | `10` |
| `coast_max_seconds` | `2` |
| `spring_max_seconds` | `1` |

## ui_text

| Field | Shipped default |
|---|---|
| `sign_in_action` | `"Sign in to {account}"` |
| `sign_in_hint` | `"Press Enter or click to sign in."` |
| `dashboard_title` | `"Dashboard"` |
| `dashboard_unavailable` | `"this build has no dashboard"` |
| `prompt_failure` | `"Prompt application: {reason}"` |
| `prompt_limit` | `"draft exceeds the prompt limit"` |
| `connect_sign_out` | `"Connect to the Nexus server to sign out."` |
| `enable_sign_out` | `"Enable the sign_out operation in the client prompt policy to use /logout."` |
| `combined_message_limit` | `"The combined message exceeds 32 KB. Shorten the current line before adding it."` |
| `logout_needs_sign_in` | `"{ai} will ask you to sign in."` |
| `logout_next_account` | `"{ai} moves to {account}."` |
| `account_switch_pending` | `"{ai} has no accounts on this server, or a switch is on its way."` |
| `secret_requested_by_app` | `"asked by nexus"` |
| `secret_requested_by` | `"asked by {asker}"` |
| `account_title` | `"Account"` |
| `sign_in_title` | `"Sign in"` |
| `browser_logout_title` | `"Sign out in your browser"` |
| `sign_out_title` | `"Sign out"` |
| `browser_logout_instruction` | `"Sign out of Claude in the browser page that opened. Then return here to continue with the selected account."` |
| `browser_logout_hint` | `"If the browser already shows a sign-in page, continue. Nexus waits for your confirmation."` |
| `browser_open` | `"Open Claude"` |
| `cancel` | `"Cancel"` |
| `browser_logout_continue` | `"I’ve signed out — Continue"` |
| `logout_confirmation` | `"Sign {ai} out of {account}? This deletes its saved login."` |
| `logout_failed` | `"Could not sign out. Your account is unchanged."` |
| `no_linked_accounts` | `"No accounts are linked to this AI."` |
| `no_signed_in_account` | `"{ai} has no signed-in account."` |
| `sign_in_another` | `"Sign in to another account…"` |
| `server_title` | `"Server"` |
| `server_description` | `"The nexus-server this device connects to, as user@host."` |
| `connect_configured_account` | `"Connect to an AI with a configured account to sign in."` |
| `service_required` | `"Connect to a server and configure a service for this tab type."` |
| `renderer_unavailable` | `"The native renderer for this tab is unavailable. Check its registration."` |
| `change_server` | `"Change server"` |
| `open_ai_account` | `"Open an AI's tab to choose its account."` |
| `connect_choose_account` | `"Connect to the Nexus server to choose an account."` |
| `connect_sign_in` | `"Connect to the Nexus server to sign in."` |
| `cut_prompt` | `"{omitted} of {given} bytes were left out: the prompt holds 32 KB."` |
| `clipboard_file_failed` | `"Could not read {file} from the clipboard."` |
| `clipboard_failed` | `"Could not read the clipboard."` |
| `clipboard_empty` | `"No files could be read from the clipboard."` |
| `clipboard_size` | `"Paste individual files up to 32 MiB each."` |
| `clipboard_image_failed` | `"The clipboard image could not be read within the 32 MiB limit."` |


## Rolling performance capture

`ui_flags.performance_recording` is on by default. Every rendered frame contributes numeric wall/monotonic timestamps,
sequence number, input delay, inter-frame gap and swap/build/draw/presentation timings. No prompt, reply, path from
conversation content, account or credential is recorded. Batches are buffered for at most two seconds of rendered
frames and handed to the existing background store writer. There is no file I/O in the frame callback.

`<client-home>/performance.jsonl` and `performance.jsonl.previous` retain two files of at most 8 MiB each. Sequence
gaps identify batches coalesced under extreme storage pressure; the UI never waits for the disk. A sequence reset
marks a new client run. The last partial batch is flushed on normal shutdown. Read these files when a stutter is
reported to inspect the preceding frames, rather than starting a profiler after it has passed. The native limits
bound diagnostic memory/disk usage. Set the flag to false to stop collecting; existing recordings remain available.

User messages, including held and sending messages, are right aligned and wrap at two-thirds of the conversation width. `ui_style.person_bubble_max_percent` controls this independently of tool and information bubbles. AI replies use `transcript_style.reply_width` from the left.

`ui_flags.protect_overlay_focus` protects an open desktop overlay against unsolicited focus changes. macOS attempts
keyboard recovery before hide-on-blur; observed Command–Tab, clicks outside the overlay, sleep/session switching and
requested browser/sign-in handoffs yield normally. Recovery uses public AppKit APIs and the OS can refuse it. Windows
uses `LockSetForegroundWindow` while foreground; explicit Alt switching and clicks retain the OS escape behavior.
Protection never opens a hidden overlay. Turn it off to restore the previous immediate hide-on-blur behavior.
Linux currently builds the server and window libraries, but has no runnable graphical client backend in this checkout;
iOS focus belongs to UIKit. No focus-lock capability is claimed for those platforms.

Dashboard stays up to date on its worker while another tab is selected and while the overlay is hidden, including
on battery. Selecting it or showing the overlay renders the latest locally available page before revealing it;
neither action reloads the document or waits for a network reply. Pending requests leave the last received data
usable, and new replies paint when they arrive. `dashboard_background_updates: false` suspends hidden page tasks.
The dashboard page owns its polling cadence; no additional poll is triggered by selection. Hidden updates do not
request window frames. Security lock screens retain their protected reveal.
`opaque_history_backdrop` covers the complete history scroll viewport, including its top inset, so the underlying
window's title-bar buttons do not show through. The outer margins retain the configured overlay transparency.
The clock uses 28-point regular text, matching the tab size. Command–Up/Down scroll history to its top/bottom;
plain Up/Down continue to navigate the account selector. Both are documented in `/help`.

`prompt_selection_shortcuts` maps chords to `start` or `end`, extending the current prompt selection with its anchor preserved. The shipped bindings are Command–Shift–Left/Right. `tab_reorder` defaults to Command–Shift–PageUp/PageDown.

The shipped outer canvas below the opaque black header is `#0c0f13` at 100% opacity. `opaque_history_backdrop` is disabled so history margins use that same transparency. The prompt fill is `#272728`; the conversation-panel fill is `#15171b`. They remain independently configurable.

Shell-command and per-tool text colors default independently to `#283f55`, the scanner's unlit blue (`#86c5ff` at a linear-light gain of `0.09`).
