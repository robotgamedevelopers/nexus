# Configuration and input ownership audit

2026-11-27. Baseline: `c05f3bf`, with the work-detail/table changes still in progress separately.

The extraction is not complete. The shipped profile owns most appearance and keyboard bindings; several application decisions and defaults still reside in native code. This is a source audit, not a claim that every configurable field has been exercised on every platform.

## Evidence and scope

Reviewed `policy/rules/config.json`, `policy/rules/client.star`, `policy/rules/client/window.star`, `cmake/ui_defaults.cmake`, `src/ui_settings.h`, `src/window_app.{h,cpp}`, `src/view.cpp`, `src/frame_host.cpp`, `src/style.h`, and the desktop/iOS entry points. Compared the live applied bundle workflow with packaged-policy validation in `src/policy/host.cpp`.

The seven generated UI sections contain 171 fields: 90 metrics, seven colors, 43 strings, seven keys, 12 flags, two font settings and ten scrolling settings. A search for explicit native member reads found one unreferenced field: `ui_style.scanner_decay_seconds`. The other 170 having reads does not establish behavioral coverage. The transcript, prompt, tab, account and sound sections are additional, manually parsed configuration surfaces and are not included in that count.

## Remaining work

| Priority | Gap and evidence | Required change and acceptance |
| --- | --- | --- |
| High | Not every application input becomes a policy event. `WindowApplication::update` obtains `app.window` shortcuts from Starlark, and `promptKey` obtains `app.prompt_key` plans; `FrameHost::event`, account/menu click paths, `Frame::build` dismissal and native modal handlers still decide outcomes directly. | Define semantic input events with context (target, action, active modal, composition state), return validated native operations, and use the same route for pointer and keyboard activation. Prove a custom profile can change work activation/dismissal, menu choices and overlay dismissal without compiling. Preserve modal precedence and terminal input. |
| High | A setting is exposed without taking effect: `scanner_decay_seconds` is declared in the profile, but the baseline scanner computes only the current Gaussian head position in `Frame::transcript`. | Wire temporal decay to rendering and test two different settings at the same timestamp, including reduced motion and tab changes. Add a consumer/behavior inventory so future settings cannot silently do nothing. The current implementation branch addresses this specific defect; it is not yet qualification evidence. |
| Medium | Several defaults still originate in C++: `PromptStyle`, `TranscriptStyle`, `TabStyle`, `style.h`, scanner timing/peak values, reply width, and optional rendering defaults. The generic UI sections already generate native defaults from JSON. | Inventory every remaining native presentation default, put the chosen value in the shipped profile, and generate compatible native fallback records from the same source. Distinguish numerical safety bounds and transport limits from user preferences. Test absent fields and old profiles as well as customized values. |
| Medium | Presentation decisions remain native: tool grouping/filtering and tool-name special cases in `itemsOf`/`declareItem`, reply/notice/work width choices, modal dimensions, and some user-facing sign-in text. | Expose public transcript presentation and semantic actions through documented profile contracts. Keep credential/private-event redaction native and unconditional. Make warning widths, work previews, detail-view dimensions, outline colors and labels independently configurable. The current UI branch covers part of this list. |
| Medium | Input tuning and priority are mixed with mechanics. `FrameHost::watchTap` has a fixed 10-point tap threshold; prompt cursor restoration, selector actions and context precedence are implemented directly in the view. | Put application gesture thresholds and intent priorities in policy, with documented defaults. Keep UTF-8 editing, IME composition, OS event delivery, focus mechanics and safe native bounds in Prax/native code. Exercise a customized profile through the actual client entry point, not just the Starlark evaluator. |
| High | Publishing a new applied policy separately from an older packaged binary produces the observed mismatch warning. Adding generated UI fields also changes the native accepted-field schema. | Stage a binary and policy from one immutable commit, verify their policy identities before restart, then install together with rollback. Keep fast live preview for compatible configuration-only edits; mark preview status explicitly and close it with a matching package. Prove warning-free startup and reject a deliberately mismatched pair. |

## What already belongs to the profile

The client Starlark module controls shortcut maps, tab registration/grouping, ordered panels, prompt send/hold behavior, command help, account selection intents, clocks and the generated appearance sections. The server independently validates authoritative operations. This is a useful foundation, not a completed migration of all input behavior.

The current UI work adds profile fields for the compact work-line count, detail-view width/height/title, output border, and animation tuning. Until the native build and functional checks pass, these remain implementation work, not delivered audit closure.

## Native responsibilities to retain

Prax/native code should continue to implement text layout, painting, clipping, scrolling, hit testing, keyboard/IME editing, platform focus, accessibility and event delivery. The native server remains authoritative for credentials, persistence, admission and protocol validation. Starlark chooses supported application actions and presentation; it cannot weaken those safety contracts or execute arbitrary native behavior.

## Completion gate

A setting is complete only when its default, schema/range, consuming primitive, customization example and behavioral check are recorded. An input action is complete only when pointer and keyboard routes reach the same validated intent with modal and composition precedence tested. Keep platform qualification and installed-build identity separate from source-level completion. The roadmap records the six remaining workstreams rather than declaring this audit a completed extraction.
