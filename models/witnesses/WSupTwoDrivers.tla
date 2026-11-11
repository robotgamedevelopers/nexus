----------------------------- MODULE WSupTwoDrivers -----------------------------
\* Reversed witness: a driver that does not say hello in time is replaced at once by a new one, without ending it and
\* waiting for its exit. OneDriver must reject it.
EXTENDS MCSupervisor

StartAgain(x) == [x EXCEPT !.procs = @ + 1, !.finished = FALSE, !.resumed = x.session]

BrokenSpec == Init /\ [][NextWith(Prompt, Message, Exit, TimeoutWith(StartAgain, End), Restart)]_vars
=============================================================================
