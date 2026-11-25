------------------------ MODULE WTermInputAfterEnd ------------------------
\* Broken ServerTerminals: input to a terminal whose shell ended is written anyway (to a reaped pseudoterminal).
\* Caught by NoInputUnlessRunning.
EXTENDS ServerTerminals

BrokenEffect(c, a) ==
    IF a.kind = "input" /\ Live(a.slot) /\ term[a.slot].ep = a.ep
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
