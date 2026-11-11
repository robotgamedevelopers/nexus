-------------------------- MODULE WDrvDoneLeftOpen --------------------------
\* Reversed witness: a stop before the prompt reached Claude writes the turn's done but leaves the turn open (finish
\* forgetting to close it), so its prompt still goes in and its result writes a second done. OneDone must reject it.
EXTENDS ClaudeDriver

DoneLeftOpen ==
    /\ wrote' = CloseText \o <<W("done-stopped", open)>>
    /\ dones' = [dones EXCEPT ![open] = @ + 1]
    /\ UNCHANGED <<driver, sent, want, turnWant>>

BrokenStop(n) == StopWith(n, DoneLeftOpen, StopRunning(n))

BrokenSpec == Init /\ [][NextWith(BrokenStop, Apply, Result, Refuse, NotFound)]_vars
=============================================================================
