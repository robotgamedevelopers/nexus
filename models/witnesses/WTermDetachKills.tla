------------------------- MODULE WTermDetachKills -------------------------
\* Broken ServerTerminals: a window unlinking ends the terminals it made (a shell tied to its link, as ssh's own
\* would be). Caught by DetachKeepsRunning.
EXTENDS ServerTerminals

BrokenDetach(c) ==
    /\ linked[c]
    /\ linked' = [linked EXCEPT ![c] = FALSE]
    /\ term' = [s \in Slots |-> IF term[s].state = "running" THEN [term[s] EXCEPT !.state = "ended"] ELSE term[s]]
    /\ UNCHANGED <<epoch, outbox, applied, delivered, seen, seenEp, restarts>>

BrokenSpec == Init /\ [][NextWith(Apply, BrokenDetach, Sync)]_vars

=============================================================================
