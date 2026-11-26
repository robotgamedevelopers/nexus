------------------------- MODULE WAppendRenamesAI -------------------------
\* Reversed witness: append also rewrites the AI's entry in the AI list
\* (its model becomes ""). AIsFixed must reject it.
EXTENDS MCWindow

BrokenAppend(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, e)]
         /\ ais' = [ais EXCEPT ![At(i)].model = ""]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<order, active, draft, queue>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, BrokenAppend, MoveTab)]_vars
=============================================================================
