----------------------------- MODULE WSignInGateTaken -----------------------------
\* Reversed witness: a window opens a second flow's page while its one gate waits for the first's callback, and binds
\* the gate for the second. The first page's redirect then finds no gate for it, and its sign-in cannot finish.
\* GateForEveryPage must reject it.
EXTENDS MCSignIn

BrokenSpec == Init /\ [][ServerNextWith(Need, Receive, Print, Timeout) \/
                         WindowNextOpening(LAMBDA c : OpenWith(LAMBDA x : TRUE, c), Redirect, Stray)]_vars
=============================================================================
