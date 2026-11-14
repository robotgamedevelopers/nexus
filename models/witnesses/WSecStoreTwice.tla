----------------------------- MODULE WSecStoreTwice -----------------------------
\* Reversed witness: a second answer for a request already stored overwrites the file ("the latest paste wins").
\* AtMostOneStored must reject it (two windows answer one request).
EXTENDS MCSecrets

StoreAgain(m) ==
    /\ file' = [file EXCEPT ![req[m.id].acct] = m.v]
    /\ stores' = [stores EXCEPT ![m.id] = @ + 1]
    /\ log' = {[id |-> m.id, event |-> "stored"]}
    /\ UNCHANGED <<req, replies>> /\ transcript' = {}

Late(m) == IF m.kind = "secret" /\ req[m.id].st = "stored" THEN StoreAgain(m) ELSE DropLate(m)

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(StoreOpen, Late, m), Timeout, Restart)]_vars
=============================================================================
