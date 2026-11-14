---------------------------- MODULE WSupAccountCuts ----------------------------
\* Reversed witness: a change of the account ends the driver at once, cutting the turn it is answering.
\* AccountKeepsTurn must reject it.
EXTENDS MCSupervisor

AtOnce(x, a) ==
    LET y == [x EXCEPT !.account = a] IN
    Advance(IF y.phase \in {"working", "stopping"} THEN End(y) ELSE y)

BrokenSpec == Init /\ [][NextWithSet(Prompt, QueuePrompt, Stop, Message, Exit, Timeout, Restart, SetSetting,
                                     LAMBDA a : SetAccountWith(AtOnce, a))]_vars
=============================================================================
