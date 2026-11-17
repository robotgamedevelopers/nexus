---------------------------- MODULE WSignInLateDropped ----------------------------
\* Reversed witness: a token the process prints after its flow ended (the deadline passed while the browser was sent
\* back, or in the grace after the process was told to end) is dropped, so the person who signed in must sign in again.
\* EveryPrintTaken must reject it.
EXTENDS MCSignIn

PrintDropsLate(a, v) ==
    /\ proc[a].st = "got-code"
    /\ LET i == proc[a].id IN
       IF flow[i].st \in Running
       THEN Print(a, v)
       ELSE /\ said' = [said EXCEPT ![i] = v]
            /\ proc' = [proc EXCEPT ![a] = NoProc] /\ procs' = [procs EXCEPT ![a] = @ - 1]
            /\ log' = {[id |-> i, event |-> "late"]}
            /\ UNCHANGED <<next, flow, cbs, badcb, signedin, losses, up, file, held, disk, failures, taken, lost,
                           restarts, ask>>
            /\ UNCHANGED win

BrokenSpec == Init /\ [][ServerNextWith(Need, Receive, PrintDropsLate, Timeout) \/ WindowNextWith(Redirect, Stray)]_vars
=============================================================================
