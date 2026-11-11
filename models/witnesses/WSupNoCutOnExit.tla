---------------------------- MODULE WSupNoCutOnExit ----------------------------
\* Reversed witness: a driver that exits with a turn open is forgotten, and the turn is left open with no driver
\* working on it: the person is never told it was cut short. OneTurn must reject it.
EXTENDS MCSupervisor

Forget(x) == [x EXCEPT !.phase = "none", !.stage = "none", !.procs = @ - 1]

BrokenSpec == Init /\ [][NextWith(Prompt, Message, ExitWith(Forget), Timeout, Restart)]_vars
=============================================================================
