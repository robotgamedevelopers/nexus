-------------------------- MODULE WLinkTailMisaligned --------------------------
\* Reversed witness: the server's tail names its first entry one index too early (the entries are the newest W, but
\* `first` says they start one before), so a window places them one entry off. ViewMatches must reject it.
EXTENDS MCLink

BrokenFirst(s) == Max(0, Len(s) - W - 1)
BrokenSnap(trs, qs, ops) == [t \in TabNames |-> LET s == trs[t] IN
                                                [first   |-> BrokenFirst(s),
                                                 entries |-> SubSeq(s, TailFirst(s) + 1, Len(s)) \o OpenOf(ops, t),
                                                 queue   |-> qs[t]]]

BrokenApplier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, server>>
    ELSE LET r == ApplyAll(c, applied[c] + 1, lst, [trs |-> tr, qs |-> q, sts |-> stops]) IN
         /\ applied' = [applied EXCEPT ![c] = lst]
         /\ done' = [done EXCEPT ![c] = @ \o Numbers(applied[c] + 1, lst)]
         /\ tr' = r.trs
         /\ q' = r.qs
         /\ stops' = r.sts
         /\ snaps' = Append(snaps, BrokenSnap(r.trs, r.qs, op))
         /\ UNCHANGED <<op, made>>

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec == Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
=============================================================================
