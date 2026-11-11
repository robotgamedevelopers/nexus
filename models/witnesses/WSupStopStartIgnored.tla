------------------------ MODULE WSupStopStartIgnored ------------------------
\* Reversed witness: a stop while the driver starts does nothing, so the turn about to begin begins anyway.
\* StopEndsWhatRuns must reject it.
EXTENDS MCSupervisor

Ignore(x) == x

BrokenSpec == Init /\ [][NextWithAll(Prompt, QueuePrompt, StopWith(Working, Ignore), Message, Exit, Timeout,
                                     Restart)]_vars
=============================================================================
