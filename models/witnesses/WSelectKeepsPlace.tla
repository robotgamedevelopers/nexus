-------------------------- MODULE WSelectKeepsPlace --------------------------
\* Reversed witness: select adds its tab's id to the end of the history only
\* when the id is not in it yet, so a tab selected again keeps its old place.
\* SelectMovesToEnd must reject it.
EXTENDS MCWindow

Recorded(h, f) == IF \E k \in 1 .. Len(h) : h[k] = f THEN h ELSE Append(h, f)

BrokenSelect(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ history' = Recorded(history, At(i))
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
