----------------------------- MODULE WSecLogValue -----------------------------
\* Reversed witness: an answer that arrives for a request no longer open is logged with what it carried, to help
\* debugging. NoSecretInLog must reject it (two windows answer one request; the second answer is late).
EXTENDS MCSecrets

LeakyLate(m) ==
    /\ log' = {[id |-> m.id, event |-> "late", value |-> m.v]}
    /\ UNCHANGED <<req, file, stores, replies, held>> /\ transcript' = {}

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(StoreOpen, LeakyLate, m), Timeout, Restart)]_vars
=============================================================================
