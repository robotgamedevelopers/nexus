------------------------------ MODULE WVoiceSpeaksAnything ------------------------------
\* Reversed witness: a reply from an AI never asked by voice is spoken. SpeaksAnswers must reject it.
EXTENDS MCVoiceTurn

BadReply(f) ==
    /\ reply # "text" /\ f \notin awaiting
    /\ Ends(phase, speaking, Append(queue, f), NoActs, "reply", f)
    /\ UNCHANGED <<permission, reply, awaiting, heard>>
BrokenSpec == Init /\ [][Next \/ \E f \in AIs : BadReply(f)]_vars /\ WF_vars(Tick)
=============================================================================
