--------------------------- MODULE WSupSendWhileOpen ---------------------------
\* Reversed witness: a prompt that arrives while a turn is open is written at once, as a second open turn, instead of
\* waiting for the open turn's done. OneTurn must reject it.
EXTENDS MCSupervisor

WriteAnyway(x) ==
    LET y == Advance(x) IN
    IF y.phase = "working" /\ y.waiting # <<>>
    THEN WriteNext([y EXCEPT !.phase = "idle"])
    ELSE y

BrokenSpec == Init /\ [][NextWith(PromptWith(WriteAnyway), Message, Exit, Timeout, Restart)]_vars
=============================================================================
