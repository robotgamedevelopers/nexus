----------------------------- MODULE WSecMalformedStored -----------------------------
\* Reversed witness: no check of the value's shape, so a paste of a sentence or of two lines is written to the account's
\* file, to be refused later by the harness. NoMalformedStored must reject it.
EXTENDS MCSecrets

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(LAMBDA x : Write(x.id, x.v), DropLate, m),
                                  Timeout, Restart) \/ RetryNext \/ DiskNext]_vars
=============================================================================
