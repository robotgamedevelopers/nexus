----------------------------- MODULE WSecHeldNeverRetried -----------------------------
\* Reversed witness: a value whose write failed is held, but never written again, even once the disk is fixed; its asker
\* waits for ever and the account never signs in. HeldIsStored must reject it.
EXTENDS MCSecrets

BrokenSpec == Init /\ [][NextWith(Ask, Sync, Receive, Timeout, Restart) \/ DiskNext]_vars
                   /\ \A r \in Ids : WF_vars(Timeout(r))
                   /\ \A c \in Clients : WF_vars(Sync(c))
                   /\ WF_vars(\E m \in up : Receive(m))
                   /\ WF_vars(DiskHeals)
=============================================================================
