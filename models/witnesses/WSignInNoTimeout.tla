----------------------------- MODULE WSignInNoTimeout -----------------------------
\* Reversed witness: a flow has no deadline. A person who never finishes on the page leaves its process running and the
\* account held by it for good. RunningEnds must reject it.
EXTENDS MCSignIn

NoDeadline(i) == i \notin Ids

BrokenSpec ==
    /\ Init
    /\ [][ServerNextWith(Need, Receive, Print, NoDeadline) \/ WindowNextWith(Redirect, Stray)]_vars
    /\ WF_vars(\E m \in up : Receive(m))
    /\ \A c \in Clients : WF_vars(Sync(c))
=============================================================================
