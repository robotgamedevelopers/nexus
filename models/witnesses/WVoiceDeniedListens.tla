------------------------------ MODULE WVoiceDeniedListens ------------------------------
\* Reversed witness: a refusal starts the microphone all the same. DeniedNeverListens must reject it.
EXTENDS MCVoiceTurn

BadRefusal ==
    /\ phase = "asking"
    /\ permission' = "refused"
    /\ Listen("permission")
    /\ UNCHANGED <<reply, awaiting>>
BrokenSpec == Init /\ [][Next \/ BadRefusal]_vars /\ WF_vars(Tick)
=============================================================================
