------------------------ MODULE WDrvStoppedLostHangs ------------------------
\* Reversed witness (the driver before this model): Claude reports the --resume id unknown while a turn is being
\* stopped, and the turn waits to start again in the new session, which a stop has already ruled out, so it never
\* gets its done. EveryTurnEnds must reject it.
EXTENDS ClaudeDriver

BrokenNotFound == NotFoundOn(inUse)
BrokenResult(k) == ResultWith(k, Refuse, BrokenNotFound)

BrokenSpec ==
    Init /\ [][NextWith(Stop, Apply, BrokenResult, Refuse, BrokenNotFound)]_vars
    /\ WF_vars(phase = "checking" /\ (CheckPass \/ Refuse \/ Fail \/ BrokenNotFound))
    /\ WF_vars(Apply) /\ WF_vars(Push) /\ WF_vars(Take) /\ WF_vars(Answer)
    /\ WF_vars(\E k \in ResultKinds : BrokenResult(k))
=============================================================================
