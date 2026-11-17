----------------------------- MODULE WSignInTokenInLog -----------------------------
\* Reversed witness: the capture's log line carries the token it captured. NoTokenInLog must reject it.
EXTENDS MCSignIn

PrintLeaks(a, v) ==
    /\ proc[a].st = "got-code"
    /\ LET i == proc[a].id IN
       /\ flow' = [flow EXCEPT ![i].st = "captured", ![i].opener = None]
       /\ taken' = [taken EXCEPT ![i] = v]
       /\ said' = [said EXCEPT ![i] = v]
       /\ log' = {[id |-> i, event |-> "captured", value |-> v]}
    /\ proc' = [proc EXCEPT ![a] = NoProc] /\ procs' = [procs EXCEPT ![a] = @ - 1]
    /\ WriteCorrect(a, v)
    /\ UNCHANGED <<next, cbs, badcb, losses, up, disk, failures, lost, restarts>> /\ UNCHANGED win

BrokenSpec == Init /\ [][ServerNextWith(Need, Receive, PrintLeaks, Timeout) \/ WindowNextWith(Redirect, Stray)]_vars
=============================================================================
