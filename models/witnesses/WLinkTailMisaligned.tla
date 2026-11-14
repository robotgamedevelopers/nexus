-------------------------- MODULE WLinkTailMisaligned --------------------------
\* Reversed witness: the server's tail names its first entry one index too early (the entries are the newest W, but
\* `first` says they start one before), so a window places them one entry off. ViewMatches must reject it.
EXTENDS MCLink

BrokenFirst(s) == Max(0, Len(s) - W - 1)
BrokenSnap(sess, trs, qs, ops) ==
    [i \in Ids |-> IF sess[i] = NoSes THEN NoSnap
                   ELSE LET s == trs[i] IN
                        [friend  |-> sess[i].friend,
                         first   |-> BrokenFirst(s),
                         entries |-> SubSeq(s, TailFirst(s) + 1, Len(s)) \o OpenOf(ops, i),
                         queue   |-> qs[i]]]

BrokenApplier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, server>>
    ELSE LET r == ApplyAll(c, applied[c] + 1, lst, Server) IN
         /\ applied' = [applied EXCEPT ![c] = lst]
         /\ done' = [done EXCEPT ![c] = @ \o Numbers(applied[c] + 1, lst)]
         /\ tr' = r.trs
         /\ q' = r.qs
         /\ stops' = r.sts
         /\ ses' = r.ses
         /\ nextSid' = r.nxt
         /\ minted' = r.mint
         /\ skipped' = r.skip
         /\ snaps' = Append(snaps, BrokenSnap(r.ses, r.trs, r.qs, op))
         /\ UNCHANGED <<op, made>>

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec == Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
=============================================================================
