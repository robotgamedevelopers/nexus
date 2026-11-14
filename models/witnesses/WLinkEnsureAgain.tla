--------------------------- MODULE WLinkEnsureAgain ---------------------------
\* Reversed witness: an ensure makes a session whether or not the friend has one, so a window's ensure at launch
\* (sent again by a second window, or after a restart) gives the friend a second session. EnsureMakesOne must reject it.
EXTENDS MCLink

EnsureAlways(r, c, n) ==
    LET a == acts[c][n] IN
    IF a.kind = "Ensure" /\ r.nxt <= MaxSessions THEN Mint(r, a.friend, c, n) ELSE ApplyOneWith(Mint, c, n, r)

RECURSIVE BrokenAll(_, _, _, _)
BrokenAll(c, from, to, r) == IF from > to THEN r ELSE BrokenAll(c, from + 1, to, EnsureAlways(r, c, from))

BrokenApplier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, server>>
    ELSE /\ applied' = [applied EXCEPT ![c] = lst]
         /\ done' = [done EXCEPT ![c] = @ \o Numbers(applied[c] + 1, lst)]
         /\ Took(BrokenAll(c, applied[c] + 1, lst, Server))

BrokenDeliver(c, first, lst) == DeliverWith(BrokenApplier, c, first, lst)

BrokenSpec == Init /\ [][NextWith(BrokenDeliver, Receive)]_vars
=============================================================================
