------------------------- MODULE WSelectAppendsAgain -------------------------
\* Reversed witness: select appends its tab's id to the history without taking
\* it out of where it stood, so a tab selected twice is in it twice.
\* HistoryOnce must reject it.
EXTENDS MCWindow

BrokenSelect(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ history' = Append(history, At(i))
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
