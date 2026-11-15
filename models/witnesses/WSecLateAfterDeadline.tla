----------------------------- MODULE WSecLateAfterDeadline -----------------------------
\* Reversed witness: an answer that crossed its request's deadline is dropped as late, though no newer request for the
\* account was ever open, so the person who pasted it must paste again. ReceivedNeverDropped must reject it.
EXTENDS MCSecrets

OpenOnly(m) == IF req[m.id].st = "expired" THEN DropLate(m) ELSE Accept(m)

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(OpenOnly, DropLate, m), Timeout, Restart)
                          \/ RetryNext \/ DiskNext]_vars
=============================================================================
