-------------------------- MODULE WAppendWrongTab --------------------------
\* Reversed witness: append puts the entry on the next tab (wrapping) instead
\* of the named one. AppendExact must reject it.
EXTENDS MCWindow

BrokenAppend(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![(i + 1) % N] = Append(@, e)]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<friends, active, draft>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, BrokenAppend)]_vars
=============================================================================
