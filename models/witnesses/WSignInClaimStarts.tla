---------------------------- MODULE WSignInClaimStarts ----------------------------
\* Reversed witness: every claim of a flow no window holds starts `claude setup-token`, a running one too. A window that
\* takes over a flow its first window let go of (a second window, or the same one reconnected) starts a second process
\* for the account. OneProcessPerAccount must reject it.
EXTENDS MCSignIn

ClaimStarts(m) ==
    LET i == m.id
        a == flow[i].acct
    IN IF flow[i].st = "needed" \/ (flow[i].st \in Running /\ flow[i].opener = None)
       THEN /\ flow' = [flow EXCEPT ![i].st = "started", ![i].opener = m.c]
            /\ proc' = [proc EXCEPT ![a] = [st |-> "running", id |-> i, ending |-> FALSE]]
            /\ procs' = [procs EXCEPT ![a] = @ + 1]
            /\ log' = {[id |-> i, event |-> "started"]}
       ELSE ClaimCorrect(m)

BrokenSpec == Init /\ [][ServerNextWith(Need, LAMBDA m : ReceiveWith(ClaimStarts, OpenedCorrect, CallbackCorrect, m),
                                         Print, Timeout) \/ WindowNextWith(Redirect, Stray)]_vars
=============================================================================
