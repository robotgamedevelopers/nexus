------------------------ MODULE WSetDraftOutsideDrafts ------------------------
\* Reversed witness: set_draft stores a text that is not one of the drafts it was
\* given (it appends a character). TypeOK must reject it.
EXTENDS MCWindow

BrokenSetDraft(i, t) ==
    IF i \in Tabs
    THEN /\ draft' = [draft EXCEPT ![At(i)] = t \o "!"]
         /\ last' = Did("set_draft", i, t, NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, transcript, queue>>
    ELSE /\ last' = Did("set_draft", i, t, NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, BrokenSetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
