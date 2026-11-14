---------------------------- MODULE WLinkStopTwice ----------------------------
\* Reversed witness: the hub hands a stop to the session's responder twice. ServerHoldsEach must reject it: each stop
\* applied reaches the session exactly once.
EXTENDS MCLink

BrokenOne(c, n, r) ==
    LET a == acts[c][n]
        s == [a |-> <<c, n>>, ok |-> Held(r.ses, a.sid)]
    IN IF a.kind = "Stop" THEN [r EXCEPT !.sts[a.sid] = @ \o << s, s >>] ELSE ApplyOne(c, n, r)

RECURSIVE BrokenAll(_, _, _, _)
BrokenAll(c, from, to, r) == IF from > to THEN r ELSE BrokenAll(c, from + 1, to, BrokenOne(c, from, r))

BrokenApplier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, server>>
    ELSE /\ applied' = [applied EXCEPT ![c] = lst]
         /\ done' = [done EXCEPT ![c] = @ \o Numbers(applied[c] + 1, lst)]
         /\ Took(BrokenAll(c, applied[c] + 1, lst, Server))

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec == Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
=============================================================================
