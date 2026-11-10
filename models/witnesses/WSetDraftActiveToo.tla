------------------------ MODULE WSetDraftActiveToo ------------------------
\* Reversed witness: set_draft writes the text to the active tab's draft as
\* well as the named tab's. SetDraftExact must reject it.
EXTENDS MCWindow

BrokenSetDraft(i, t) ==
    IF i \in Tabs
    THEN /\ draft' = [draft EXCEPT ![At(i)] = t, ![At(active)] = t]
         /\ last' = Did("set_draft", i, t, NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, transcript, queue>>
    ELSE /\ last' = Did("set_draft", i, t, NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, BrokenSetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
