----------------------------- MODULE WSecNoTimeout -----------------------------
\* Reversed witness: a request has no deadline, so one nobody answers (the person away, or every window gone) stays
\* open for ever and its asker waits for ever. EveryRequestEnds must reject it.
EXTENDS MCSecrets

BrokenSpec == Init /\ [][NextWith(Ask, Sync, Receive, LAMBDA r : FALSE, Restart)]_vars
                   /\ \A c \in Clients : WF_vars(Sync(c))
                   /\ WF_vars(\E m \in up : Receive(m))
=============================================================================
