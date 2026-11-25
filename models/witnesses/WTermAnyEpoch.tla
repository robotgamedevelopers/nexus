--------------------------- MODULE WTermAnyEpoch ---------------------------
\* Broken ServerTerminals: input is written to the terminal in its slot whatever run it was typed for (ids without
\* the epoch). Caught by InputOnlyToItsRun.
EXTENDS ServerTerminals

BrokenEffect(c, a) ==
    IF a.kind = "input" /\ term[a.slot].state = "running"
    THEN /\ delivered' = [delivered EXCEPT ![a.slot] = Append(@, [c |-> c, key |-> a.key, ep |-> a.ep])]
         /\ UNCHANGED term
    ELSE Effect(c, a)

BrokenApply(c) ==
    /\ linked[c]
    /\ applied[c] < Len(outbox[c])
    /\ applied' = [applied EXCEPT ![c] = @ + 1]
    /\ BrokenEffect(c, outbox[c][applied[c] + 1])
    /\ UNCHANGED <<epoch, outbox, linked, seen, seenEp, restarts>>

BrokenSpec == Init /\ [][NextWith(BrokenApply, Detach, Sync)]_vars

=============================================================================
