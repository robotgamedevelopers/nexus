--------------------------- MODULE WLinkDeliverShort ---------------------------
\* Reversed witness: the Applier stops one short of the range it is given, so the last action a window sends is never
\* applied, however often mosh delivers it. Every safety property holds; the liveness property Converges must reject
\* it.
EXTENDS MCLink

BrokenApplier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst - 1 <= applied[c]
    THEN UNCHANGED <<applied, done, tr, q, snaps>>
    ELSE ApplyRange(c, applied[c] + 1, lst - 1)

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec ==
    /\ Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
    /\ \A c \in Clients : WF_vars(BrokenDeliver(c, 1, Len(acts[c]))) /\ WF_vars(ReceiveLatest(c))
=============================================================================
