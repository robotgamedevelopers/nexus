----------------------------- MODULE WSupTurnReuse -----------------------------
\* Reversed witness: the next turn number is not made durable before its send is written, so a server that restarts
\* with a turn open numbers the next turn as the one it cut. TurnsIncrease must reject it.
EXTENDS MCSupervisor

BrokenRestarted(x) == [Restarted(x) EXCEPT !.next = IF x.open # 0 THEN x.open ELSE @]

BrokenSpec == Init /\ [][NextWith(Prompt, Message, Exit, Timeout, RestartWith(BrokenRestarted))]_vars
=============================================================================
