----------------------------- MODULE WSecStoredBeforeDurable -----------------------------
\* Reversed witness: a value whose write failed is held, but its asker is told "stored" at once (as a server before the
\* one-chance rule told "stored, unconfirmed" when the directory's sync failed). StoredOnlyDurable must reject it.
EXTENDS MCSecrets

HoldTold(r, v) ==
    /\ req' = [req EXCEPT ![r].st = "holding"]
    /\ held' = [held EXCEPT ![r] = v]
    /\ file' = [file EXCEPT ![req[r].acct] = v]
    /\ replies' = replies \cup {Reply(r, "stored", "none")}
    /\ transcript' = {[id |-> r, what |-> "stored", acct |-> req[r].acct]}
    /\ log' = {[id |-> r, event |-> "stored"]}
    /\ UNCHANGED stores

Told(m) == IF Good(m.v) /\ disk = "failing" THEN HoldTold(m.id, m.v) ELSE Accept(m)

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(Told, DropLate, m), Timeout, Restart)
                          \/ RetryNext \/ DiskNext]_vars
=============================================================================
