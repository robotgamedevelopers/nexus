---------------------------- MODULE WLinkQueueAgain ----------------------------
\* Reversed witness: the mirror queues the server's whole queue each time it receives a state, not only the prompts it
\* has not queued, so a queued prompt shows twice. QueueMatches must reject it.
EXTENDS MCLink

BrokenMirror(v, qv, se, s) ==
    LET r == MirrorTab(v, qv, se, s) IN
    [r EXCEPT !.vq = qv \o s.queue]

BrokenReceive(c, j) == ReceiveWith(BrokenMirror, c, j)

BrokenSpec == Init /\ [][NextWith(Deliver, BrokenReceive)]_vars
=============================================================================
