----------------------------- MODULE WMoveSwaps -----------------------------
\* Reversed witness: move swaps the tabs at `from` and `to` instead of
\* shifting the tabs between, so on three tabs an AI between them jumps
\* past another. Every tab is kept and the active AI follows; MovePlaces
\* must reject it.
EXTENDS MCWindow

Swapped(o, from, to) == [o EXCEPT ![from] = o[to], ![to] = o[from]]

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN /\ order' = Swapped(order, from, to)
         /\ active' = (IF active = from THEN to ELSE IF active = to THEN from ELSE active)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
