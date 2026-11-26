-------------------------- MODULE WAsSwitchedEarly --------------------------
\* Reversed witness: the phone says a switch is done once it hands it to the link, before the server has it; the phone restarts and its old link never delivers it. NoSilentDrop must reject it.
EXTENDS AccountSwitch

BrokenHand ==
    /\ waiting # None
    /\ view' = server
    /\ inflight' = waiting /\ runQ' = runQ \cup {waiting} /\ linkQ' = linkQ \cup {waiting} /\ waiting' = None
    /\ switched' = Max(switched, waiting.num)
    /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, resps, seen, crashes>>

BrokenSpec == Init /\ [][Next \/ BrokenHand]_vars
=============================================================================
