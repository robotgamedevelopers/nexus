------------------------- MODULE WSupRemoveLeavesTurn -------------------------
\* Reversed witness: taking the friend out of friends.json with a turn open leaves the turn running (no stop line), as
\* a server that only moved the session to no account and no instance did: the friend works on for a friend no window
\* shows. (A driver still starting is ended as the spec says, so the counterexample is the turn.) AbsentRunsNothing must
\* reject it.
EXTENDS MCSupervisor

LeavesTurn(x) == IF x.phase = "working" THEN x ELSE Removed(x)

BrokenSpec == Init /\ [][NextRemove(LeavesTurn)]_vars
=============================================================================
