--------------------------- MODULE WSupNoStopTimeout ---------------------------
\* Reversed witness: no deadline on a stop, so a driver that ignores the stop keeps the turn open for ever, and the
\* prompt is neither answered nor cut. Settles must reject it.
EXTENDS MCSupervisor

WaitForever(x) == x

BrokenTimeout == TimeoutWith(End, WaitForever)
BrokenKilled == s.phase = "ending" /\ s.stage = "killed" /\ Exit

BrokenSpec == Init /\ [][NextWith(Prompt, Message, Exit, BrokenTimeout, Restart)]_vars
                   /\ WF_vars(BrokenTimeout) /\ WF_vars(Stop) /\ WF_vars(BrokenKilled)
=============================================================================
