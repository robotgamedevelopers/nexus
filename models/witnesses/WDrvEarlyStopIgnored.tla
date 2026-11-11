------------------------ MODULE WDrvEarlyStopIgnored ------------------------
\* Reversed witness: a stop that comes before the prompt reached Claude is ignored, so the turn runs anyway.
\* StopEndsWhatRuns must reject it.
EXTENDS ClaudeDriver

BrokenStop(n) == StopWith(n, StopIgnored, StopRunning(n))

BrokenSpec == Init /\ [][NextWith(BrokenStop, Apply, Result, Refuse, NotFound)]_vars
=============================================================================
