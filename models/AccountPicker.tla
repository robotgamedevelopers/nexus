-------------------------- MODULE AccountPicker --------------------------
EXTENDS Integers, Sequences

\* The window's local preview and the SetAccount request that one input can
\* authorize. A request does not change the session's authoritative account.
Accounts == {"a", "b"}
Tabs == {1, 2}
Sessions == {1, 2, 3}
Rosters == {<<>>, <<"a">>, <<"b">>, <<"a", "b">>, <<"b", "a">>}
Directions == {-1, 0, 1}
Names(o) == {o[i] : i \in DOMAIN o}
NoRequest == [session |-> 0, account |-> ""]
Closed == [tab |-> 0, session |-> 0, options |-> <<>>, current |-> "", highlight |-> ""]
Snapshot(t, s, o, c) == [tab |-> t, session |-> s, options |-> o, current |-> c]
VARIABLES picker, request, event, observed
vars == <<picker, request, event, observed>>
Init == picker = Closed /\ request = NoRequest /\ event = "init" /\ observed = Snapshot(0, 0, <<>>, "")

\* The selection is a name even though the rendered list highlights a row.
Index(o, id) == CHOOSE i \in DOMAIN o : o[i] = id
Advance(o, id, d) ==
    IF id \in Names(o) THEN o[1 + ((Index(o, id) - 1 + d) % Len(o))]
    ELSE IF d < 0 THEN o[Len(o)] ELSE o[1]

Open(t, s, o, c, d, ready) ==
    /\ picker = Closed /\ observed' = Snapshot(t, s, o, c)
    /\ picker' = IF ~ready \/ o = <<>> THEN Closed ELSE [tab |-> t, session |-> s, options |-> o,
                  current |-> c, highlight |-> Advance(o, c, d)]
    /\ request' = NoRequest /\ event' = "open"

Step(d) ==
    /\ picker # Closed /\ UNCHANGED observed
    /\ picker' = [picker EXCEPT !.highlight = Advance(picker.options, picker.highlight, d)]
    /\ request' = NoRequest /\ event' = "step"

Refresh(t, s, o, c) ==
    /\ observed' = Snapshot(t, s, o, c)
    /\ picker' = IF picker # Closed /\ t = picker.tab /\ s = picker.session /\ c = picker.current /\ picker.highlight \in Names(o)
                 THEN [picker EXCEPT !.options = o] ELSE Closed
    /\ request' = NoRequest /\ event' = "refresh"

\* Escape, an outside click, a modal prompt or loss of the link dismisses the preview.
Cancel == UNCHANGED observed /\ picker' = Closed /\ request' = NoRequest /\ event' = "cancel"

\* Enter passes the highlighted name. A row click passes that row's name.
\* A click on an obsolete name, choosing the current account or a second Enter does nothing.
Choose(id) ==
    /\ UNCHANGED observed
    /\ request' = IF id \in Names(picker.options) /\ id # picker.current
                  THEN [session |-> picker.session, account |-> id] ELSE NoRequest
    /\ picker' = Closed /\ event' = "choose"

Next == \/ \E t \in Tabs, s \in Sessions, o \in Rosters, c \in Accounts \cup {""}, d \in Directions, ready \in BOOLEAN : Open(t, s, o, c, d, ready)
        \/ \E d \in Directions : Step(d)
        \/ \E t \in Tabs, s \in Sessions, o \in Rosters, c \in Accounts \cup {""} : Refresh(t, s, o, c)
        \/ Cancel
        \/ \E id \in Accounts \cup {""} : Choose(id)
Spec == Init /\ [][Next]_vars

ValidPreview == picker = Closed \/ (picker.tab \in Tabs /\ picker.session \in Sessions
                 /\ picker.options \in Rosters /\ picker.current \in Accounts \cup {""}
                 /\ picker.highlight \in Names(picker.options))
LiveBinding == picker = Closed \/ Snapshot(picker.tab, picker.session, picker.options, picker.current) = observed
OnlyConfirmationSends == request = NoRequest \/ (event = "choose" /\ picker = Closed)
TargetsDisplayedSession == [][request' # NoRequest =>
    (picker # Closed /\ request'.session = observed.session
     /\ request'.account \in Names(observed.options) /\ request'.account # observed.current)]_vars
RefreshKeepsName == [][event' = "refresh" /\ picker' # Closed =>
    (picker'.highlight = picker.highlight /\ picker'.session = picker.session /\ picker'.tab = picker.tab)]_vars

\* Reversed witness: an arrow sends the previewed account before the person confirms.
Premature == /\ picker # Closed /\ picker.highlight # picker.current
             /\ event' = "step" /\ UNCHANGED <<picker, observed>>
             /\ request' = [session |-> picker.session, account |-> picker.highlight]
Broken == Init /\ [][Next \/ Premature]_vars
\* Reversed witness: the desktop keeps a preview when a replacement session arrives.
Stale == /\ picker # Closed /\ observed' = [observed EXCEPT !.session = 1 + (@ % 3)]
         /\ UNCHANGED picker /\ request' = NoRequest /\ event' = "refresh"
BrokenStale == Init /\ [][Next \/ Stale]_vars
=============================================================================
