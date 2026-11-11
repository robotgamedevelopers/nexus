--------------------------- MODULE WSupResendAfterCut ---------------------------
\* Reversed witness: a driver that exits with a turn open has the turn cut, but its prompt is put back at the head of
\* the waiting prompts, so the next driver is sent it again. AtMostOnce must reject it.
EXTENDS MCSupervisor

BrokenExited(x) ==
    LET y == ExitedWith(x) IN
    IF x.prompt # 0 /\ x.phase # "ending"
    THEN [y EXCEPT !.waiting = <<x.prompt>> \o @, !.fate[x.prompt] = "waiting"]
    ELSE y

BrokenSpec == Init /\ [][NextWith(Prompt, Message, ExitWith(BrokenExited), Timeout, Restart)]_vars
=============================================================================
