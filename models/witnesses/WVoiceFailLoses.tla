------------------------------ MODULE WVoiceFailLoses ------------------------------
\* Reversed witness: a failure drops what was heard instead of drafting it. NothingLost must reject it.
EXTENDS MCVoiceTurn

BadFail ==
    /\ phase \in {"listening", "finishing"}
    /\ Ends("idle", speaking, queue, [NoActs EXCEPT !.stopListen = (phase = "listening")], "fail", None)
    /\ UNCHANGED <<permission, reply, awaiting, heard>>
BrokenSpec == Init /\ [][Next \/ BadFail]_vars /\ WF_vars(Tick)
=============================================================================
