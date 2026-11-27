# Repeating session goals

`/goal <text>` sends the literal text immediately and repeats it every five minutes in this session, through whichever
harness the session uses. Setting another goal replaces the old one and restarts the interval. `/goal` shows its status;
`/goal off` stops future repetitions. Command–Delete also stops the goal while clearing the composer and held message.
A turn already submitted is not recalled; Command–Escape stops current work.

The server owns and persists the goal and its next deadline. It continues while the client is hidden or disconnected,
including on a headless server. Reconnecting displays the authoritative goal again. A restart or missed interval sends
at most one reminder, never a catch-up burst. If the same goal is already waiting for the harness, no duplicate is added.
A new session starts without a goal; changing the account or harness of this session retains its goal.

An active goal stays in a bubble below the held message and above the prompt box, outside the scrolling transcript.
Its thick solid orange outline uses the cursor's pulse, HDR headroom and reduced-motion/power behavior. The bubble
remains until the server confirms cancellation; losing the connection cannot falsely claim that a goal was stopped.
The help command documents goal commands, cancellation, and the current command and keyboard bindings.

The goal text, deadline and resulting turn changes commit together before driver effects. Timed delivery bypasses
slash-command parsing: a goal containing `/clear` is text for the harness, never a Nexus command. Existing message
size, admission and authentication checks still apply. A refused timed delivery reports its reason and waits another
interval before retrying. A store failure does not advance the deadline or dispatch the turn.
