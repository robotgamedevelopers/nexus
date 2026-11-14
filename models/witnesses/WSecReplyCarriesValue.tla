------------------------- MODULE WSecReplyCarriesValue -------------------------
\* Reversed witness: the server answers the asker "stored" with the value it stored in the reply. NoSecretToAsker must
\* reject it.
EXTENDS MCSecrets

LeakyStore(m) ==
    LET r == m.id IN
    /\ req' = [req EXCEPT ![r].st = "stored"]
    /\ file' = [file EXCEPT ![req[r].acct] = m.v]
    /\ stores' = [stores EXCEPT ![r] = @ + 1]
    /\ replies' = replies \cup {Reply(r, "stored", m.v)}
    /\ transcript' = {[id |-> r, what |-> "stored", acct |-> req[r].acct]}
    /\ log' = {[id |-> r, event |-> "stored"]}
    /\ UNCHANGED held

BrokenSpec == Init /\ [][NextWith(Ask, Sync, LAMBDA m : ReceiveWith(LeakyStore, DropLate, m), Timeout, Restart)]_vars
=============================================================================
