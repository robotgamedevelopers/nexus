----------------------------- MODULE WSupStartFresh -----------------------------
\* Reversed witness: a driver is started without --resume although a session id is known, so the friend loses the
\* conversation. StartsSound must reject it.
EXTENDS MCSupervisor

Fresh2(x) ==
    LET y == Advance(x) IN
    IF y.procs > x.procs THEN [y EXCEPT !.resumed = ""] ELSE y

BrokenSpec == Init /\ [][NextWith(PromptWith(Fresh2), Message, Exit, Timeout, Restart)]_vars
=============================================================================
