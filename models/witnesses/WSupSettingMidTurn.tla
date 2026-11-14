--------------------------- MODULE WSupSettingMidTurn ---------------------------
\* Reversed witness: a change of the model or effort while a turn is open writes the open turn's prompt again with the
\* new setting, so the turn changes model halfway. SettingLeavesTurn must reject it.
EXTENDS MCSupervisor

MidTurn(x, v) ==
    LET y == [x EXCEPT !.setting = v] IN
    IF y.open # 0 THEN [y EXCEPT !.written = Append(@, [p |-> y.prompt, t |-> y.open, v |-> v, sum |-> FALSE])] ELSE y

BrokenSpec == Init /\ [][NextWithSet(Prompt, QueuePrompt, Stop, Message, Exit, Timeout, Restart,
                                     LAMBDA v : SetSettingWith(MidTurn, v), SetAccount)]_vars
=============================================================================
