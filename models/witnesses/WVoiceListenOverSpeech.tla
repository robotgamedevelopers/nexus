------------------------------ MODULE WVoiceListenOverSpeech ------------------------------
\* Reversed witness: listening starts with the reply still being spoken. NeverHearsItself must reject it.
EXTENDS MCVoiceTurn

BadListen(op) ==
    /\ phase' = "listening" /\ heard' = FALSE /\ UNCHANGED <<speaking, queue>>
    /\ last' = [op |-> op, ai |-> None, acts |-> [NoActs EXCEPT !.start = TRUE]]
BadPress ==
    IF phase \in {"idle", "denied"} /\ permission = "granted"
    THEN BadListen("press") /\ UNCHANGED <<permission, reply, awaiting>>
    ELSE Press
BrokenSpec == Init /\ [][Next \/ BadPress]_vars /\ WF_vars(Tick)
=============================================================================
