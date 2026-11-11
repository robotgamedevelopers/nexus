---------------------------- MODULE WLinkTakeNewest ----------------------------
\* Reversed witness: at a turn's end the server delivers the newest queued prompt, not the oldest. OnlyGrow must
\* reject it: a queue loses only its oldest prompt.
EXTENDS MCLink

TakenNewest(trt, qt) ==
    [tr |-> Append(trt, Prompt(qt[Len(qt)][1], qt[Len(qt)][2])), q |-> SubSeq(qt, 1, Len(qt) - 1)]
BrokenTake(t) == TakeWith(TakenNewest, t)

BrokenSpec == Init /\ [][NextWithAll(Deliver, Receive, Close, BrokenTake, Restart)]_vars
=============================================================================
