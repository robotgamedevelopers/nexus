----------------------------- MODULE WSecLateStored -----------------------------
\* Reversed witness: an answer that arrives after its request was cancelled (refused) is stored anyway, since the
\* person did paste it. ClosedStaysClosed must reject it: the asker was already told "refused".
EXTENDS MCSecrets

Late(m) == IF m.kind = "secret" /\ req[m.id].st = "refused" THEN StoreOpen(m) ELSE DropLate(m)

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(StoreOpen, Late, m), Timeout, Restart)]_vars
=============================================================================
