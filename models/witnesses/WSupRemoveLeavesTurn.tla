------------------------- MODULE WSupRemoveLeavesTurn -------------------------
\* Reversed witness: taking the AI out of ais.json with a turn open leaves the turn running (no stop line), as
\* a server that only moved the session to no account and no instance did: the AI works on for an AI no window
\* shows. (A driver still starting is ended as the spec says, so the counterexample is the turn.) AbsentRunsNothing must
\* reject it.
EXTENDS MCSupervisor

LeavesTurn(x) == IF x.phase = "working" THEN x ELSE Removed(x)

BrokenSpec == Init /\ [][NextRemove(LeavesTurn)]_vars
=============================================================================
