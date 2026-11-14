-------------------------- MODULE WSecTranscriptValue --------------------------
\* Reversed witness: the line the asker's session shows when the secret is stored carries the value. Its AI reads the
\* transcript back, so NoSecretInTranscript must reject it.
EXTENDS MCSecrets

LeakyStore(m) ==
    LET r == m.id IN
    /\ req' = [req EXCEPT ![r].st = "stored"]
    /\ file' = [file EXCEPT ![req[r].acct] = m.v]
    /\ stores' = [stores EXCEPT ![r] = @ + 1]
    /\ replies' = replies \cup {Reply(r, "stored", "none")}
    /\ transcript' = {[id |-> r, what |-> "stored", acct |-> req[r].acct, value |-> m.v]}
    /\ log' = {[id |-> r, event |-> "stored"]}
    /\ UNCHANGED held

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(LeakyStore, DropLate, m), Timeout, Restart)]_vars
=============================================================================
