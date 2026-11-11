---------------------------- MODULE WLinkAcceptGap ----------------------------
\* Reversed witness: the Applier takes a range that starts past applied + 1, skipping the actions between, instead of
\* refusing it. ExactlyOnce must reject it.
EXTENDS MCLink

BrokenApplier(c, first, lst) ==
    IF lst <= applied[c]
    THEN UNCHANGED <<applied, done, server>>
    ELSE ApplyRange(c, Max(first, applied[c] + 1), lst)

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec == Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
=============================================================================
