---------------------------- MODULE WSupStopAgain ----------------------------
\* Reversed witness: a stop is written for the open turn whenever the person stops, even while an earlier stop of the
\* same turn is out, so a turn gets two. StopOnce must reject it.
EXTENDS MCSupervisor

Open(x) == x.phase \in {"working", "stopping"}

BrokenSpec == Init /\ [][NextWithAll(Prompt, QueuePrompt, StopWith(Open, CancelStart), Message, Exit, Timeout, Restart)]_vars
=============================================================================
