------------------------------ MODULE WVoiceTextSpeaks ------------------------------
\* Reversed witness: choosing text leaves the reply speaking. TextIsSilent must reject it.
EXTENDS MCVoiceTurn

BadSetReply(r) ==
    /\ reply' = r
    /\ last' = [op |-> "set_reply", ai |-> None, acts |-> NoActs]
    /\ UNCHANGED <<phase, permission, speaking, queue, awaiting, heard>>
BrokenSpec == Init /\ [][Next \/ \E r \in Replies : BadSetReply(r)]_vars /\ WF_vars(Tick)
=============================================================================
