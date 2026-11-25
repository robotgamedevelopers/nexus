---------------------------- MODULE WTermReplay ----------------------------
\* Broken ServerTerminals: a window that links again has its actions applied from the first, the store's counter
\* forgotten (an Applier made with 0). Caught by InputOnceInOrder.
EXTENDS ServerTerminals

BrokenAttach(c) ==
    /\ ~linked[c]
    /\ linked' = [linked EXCEPT ![c] = TRUE]
    /\ applied' = [applied EXCEPT ![c] = 0]
    /\ UNCHANGED <<epoch, term, outbox, delivered, seen, seenEp, restarts>>

BrokenSpec == Init /\ [][Next \/ \E c \in Clients : BrokenAttach(c)]_vars

=============================================================================
