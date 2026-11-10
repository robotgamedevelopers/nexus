---------------------------- MODULE WLinkApplyAgain ----------------------------
\* Reversed witness: the Applier applies a range from its first number, not from applied + 1, so a range mosh delivers
\* again (or one that overlaps what was applied) applies those actions a second time. ExactlyOnce must reject it.
EXTENDS MCLink

BrokenApplier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, tr, q, snaps>>
    ELSE ApplyRange(c, first, lst)

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec == Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
=============================================================================
