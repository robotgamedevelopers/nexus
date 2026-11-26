------------------------------ MODULE WVoiceNoDefault ------------------------------
\* Reversed witness: a prompt naming no one, with no AI showing, goes to an AI other than the first. DefaultIsFirst
\* must reject it.
EXTENDS MCVoiceTurn

BadFinal(f) ==
    /\ Listening /\ f # First
    /\ heard' = TRUE
    /\ awaiting' = awaiting \cup {f}
    /\ Ends("idle", speaking, queue, [NoActs EXCEPT !.stopListen = (phase = "listening"), !.send = f], "final", None)
    /\ UNCHANGED <<permission, reply>>
BrokenSpec == Init /\ [][Next \/ \E f \in AIs : BadFinal(f)]_vars /\ WF_vars(Tick)
=============================================================================
