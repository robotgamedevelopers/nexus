---------------------------- MODULE WSignInExpiredKeeps ----------------------------
\* Reversed witness: a flow past its deadline expires but its `claude setup-token` is left running, still listening on
\* its port and still able to print a token for a flow that ended. ProcessHasItsFlow must reject it.
EXTENDS MCSignIn

BrokenSpec == Init /\ [][ServerNextWith(Need, Receive, Print,
                                         LAMBDA i : TimeoutWith(LAMBDA j : UNCHANGED <<proc, procs>>, i)) \/
                         WindowNextWith(Redirect, Stray)]_vars
=============================================================================
