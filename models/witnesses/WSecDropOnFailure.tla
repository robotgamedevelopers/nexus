----------------------------- MODULE WSecDropOnFailure -----------------------------
\* Reversed witness: the server before the one-chance rule. A value whose file cannot be written is refused
\* ("unwritable") and dropped, so the person must paste it again. ReceivedNeverDropped must reject it.
EXTENDS MCSecrets

DropOnFailure(m) ==
    IF Good(m.v) /\ disk = "failing" /\ req[m.id].st = "open"
    THEN RefuseOpen(m.id, "unwritable") /\ UNCHANGED <<file, stores, held>>
    ELSE Accept(m)

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(DropOnFailure, DropLate, m), Timeout, Restart)
                          \/ RetryNext \/ DiskNext]_vars
=============================================================================
