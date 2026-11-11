------------------------------ MODULE WSupTextLost ------------------------------
\* Reversed witness: a driver that exits mid-turn has its turn cut, but the text it had written for the turn is thrown
\* away instead of going into the tab before the notice. TextKept must reject it.
EXTENDS MCSupervisor

DropText(x) == ExitedWith([x EXCEPT !.gathered = FALSE])

BrokenSpec == Init /\ [][NextWith(Prompt, Message, ExitWith(DropText), Timeout, Restart)]_vars
=============================================================================
