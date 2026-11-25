--------------------------- MODULE WRewriteEntry ---------------------------
\* Reversed witness: append, on a tab that already has entries, overwrites the
\* first entry with the new one as well as appending it. TranscriptsOnlyGrow
\* must reject it.
EXTENDS MCWindow

BrokenAppend(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] =
                              IF Len(@) > 0 THEN Append([@ EXCEPT ![1] = e], e) ELSE Append(@, e)]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<ais, order, active, draft, queue>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, BrokenAppend, MoveTab)]_vars
=============================================================================
