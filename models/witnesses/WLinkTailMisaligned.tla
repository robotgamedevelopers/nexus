-------------------------- MODULE WLinkTailMisaligned --------------------------
\* Reversed witness: the server's tail names its first entry one index too early (the entries are the newest W, but
\* `first` says they start one before), so a window places them one entry off. ViewMatches must reject it.
EXTENDS MCLink

BrokenFirst(s) == Max(0, Len(s) - W - 1)
BrokenSnap(trs, qs) == [t \in TabNames |-> [first   |-> BrokenFirst(trs[t]),
                                            entries |-> SubSeq(trs[t], TailFirst(trs[t]) + 1, Len(trs[t])),
                                            queue   |-> qs[t]]]

BrokenApplier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, tr, q, snaps>>
    ELSE LET r == ApplyAll(c, applied[c] + 1, lst, tr, q) IN
         /\ applied' = [applied EXCEPT ![c] = lst]
         /\ done' = [done EXCEPT ![c] = @ \o Numbers(applied[c] + 1, lst)]
         /\ tr' = r.trs
         /\ q' = r.qs
         /\ snaps' = Append(snaps, BrokenSnap(r.trs, r.qs))

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec == Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
=============================================================================
