----------------------------- MODULE WSecReplyTwice -----------------------------
\* Reversed witness: an answer that arrives for a request no longer open is answered to the asker as refused, so an
\* asker already told "stored" hears "refused" too. AnsweredOnce must reject it.
EXTENDS MCSecrets

Late(m) ==
    /\ replies' = replies \cup {Reply(m.id, "refused", "timeout")}
    /\ log' = {[id |-> m.id, event |-> "late"]}
    /\ UNCHANGED <<req, file, stores, held>> /\ transcript' = {}

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(StoreOpen, Late, m), Timeout, Restart)]_vars
=============================================================================
